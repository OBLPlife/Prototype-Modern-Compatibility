"""Passive native-engine snapshots, on the existing collector's five-second tick.

Collector uses read-only memory/query access. The opt-in patch publishes bounded
GPU queries/wait stack samples to shared memory. No suspends, ETW or launches.
Field provenance: review-20261114-171237/engine-{render,scheduler,native}.
"""
import ctypes as C
from ctypes import wintypes as W
import json
import math
import os
from pathlib import Path
import re
import struct
import time

MAX_ENGINE_BYTES = 1_000_000  # Independent decimal MB, including headers/checkpoints.
RENDER_RVA = 0x013155d8
POOL_RVA = 0x01327d10
WORKER_ENTRY_RVA = 0x00b7ee70
# Relocation-free, unmodified code/data spans; never compare relocated pointers.
GUARDS = ((0x644fb0, bytes.fromhex('518b4c240c8b54241053568b7424108d')),
          (0x860e80, bytes.fromhex('568bf1578d7e1457e803c4ddff8b4638')),
          (WORKER_ENTRY_RVA, bytes.fromhex('81ec00020000568bb42408020000578b')),
          (0xd9fdfc, bytes.fromhex('6f12833a')))
EVENT_MARKERS = ('metrics present ', 'resources memory-valid=', '| screen |',
                 'window event=', 'display ', 'save ', 'TEST ', 'device recovery ',
                 'device lost ', 'WOI ', 'woi ', 'blackfade', 'focus-return')
CAPABILITIES = dict(
    collected='native thread roles/CPU deltas, queue snapshots/latest phase wall times, sampled GPU render/upload timestamp spans, Present/upload CPU wall spans, original engine wait API wall spans and bounded runtime return-PC stacks, existing frame/events and device GPU counters',
    unavailable=['complete per-phase CPU execution versus wait split', 'complete task dependency graph',
                 'continuous OS ready-time and driver-internal wait stacks', 'exact cause of every long frame',
                 'per-core scheduling and native animation pose frequency'],
    native_timing='latest overwritten native milliseconds, NOT five-second averages or full-frame CPU/GPU duration',
    queue_scope='instantaneous non-atomic observations, NOT occupancy averages or stall durations',
    gpu_scope='NVML is whole-device; sampled D3D GPU command spans can include gaps/preemption; neither is continuous GPU Busy',
    wait_scope='first wait per API kind/thread per ~100ms window each 5s; wall duration includes scheduling, not pure blocked time; stacks may truncate at x86 FPO frames',
    gpu_upload_scope='up to 4 timestamp pairs in selected frame around managed texture/LOD, explicit UpdateTexture and managed buffer upload APIs; not all engine/driver transfers',
    correlation='same observer tick; source frame/event windows retain their own timestamps, NOT the same exact frame',
    overhead='PENDING_GAME_AB; bounded low-frequency reads do not prove zero impact')


def u32(data, offset=0):
    return struct.unpack_from('<I', data, offset)[0]


def ascii_name(data):
    return data.split(b'\0', 1)[0].decode('ascii', 'replace')


def pointer32(value, size=1):
    if value < 0x10000 or value & 3 or value + size > 0x100000000:
        raise ValueError('invalid or uninitialized 32-bit engine pointer')
    return value


class MODULEENTRY32W(C.Structure):
    _fields_ = [('size', W.DWORD), ('module_id', W.DWORD), ('pid', W.DWORD),
                ('global_usage', W.DWORD), ('process_usage', W.DWORD),
                ('base', C.c_void_p), ('image_bytes', W.DWORD), ('module', W.HMODULE),
                ('name', W.WCHAR * 256), ('path', W.WCHAR * 260)]


class NativeReader:
    def __init__(self, pid, creation, expected_path):
        self.pid, self.creation = pid, creation
        self.base, self.handle, self.verified = None, None, False
        self.error, self.next_discovery = None, 0.0
        self.read_calls = self.read_bytes = self.read_failures = 0
        self.discovery_attempts = 0
        self.module_ranges = []
        self.k = C.WinDLL('kernel32', use_last_error=True)
        declarations = {
            'OpenProcess': ([W.DWORD, W.BOOL, W.DWORD], W.HANDLE),
            'CloseHandle': ([W.HANDLE], W.BOOL),
            'ReadProcessMemory': ([W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t,
                                   C.POINTER(C.c_size_t)], W.BOOL),
            'GetProcessTimes': ([W.HANDLE] + [C.POINTER(C.c_ulonglong)] * 4, W.BOOL),
            'QueryFullProcessImageNameW': ([W.HANDLE, W.DWORD, W.LPWSTR,
                                           C.POINTER(W.DWORD)], W.BOOL),
            'CreateToolhelp32Snapshot': ([W.DWORD, W.DWORD], W.HANDLE),
            'Module32FirstW': ([W.HANDLE, C.POINTER(MODULEENTRY32W)], W.BOOL),
            'Module32NextW': ([W.HANDLE, C.POINTER(MODULEENTRY32W)], W.BOOL),
        }
        for name, (args, result) in declarations.items():
            fn = getattr(self.k, name); fn.argtypes = args; fn.restype = result
        try:
            # Separate optional handle: denial must not disable ordinary counters.
            self.handle = self.k.OpenProcess(0x1000 | 0x10, False, pid)
            if not self.handle:
                raise C.WinError(C.get_last_error())
            born, end, kernel, user = (C.c_ulonglong() for _ in range(4))
            if not self.k.GetProcessTimes(self.handle, C.byref(born), C.byref(end),
                                         C.byref(kernel), C.byref(user)):
                raise C.WinError(C.get_last_error())
            if born.value != creation:
                raise ValueError('Original process creation time no longer matches')
            path = C.create_unicode_buffer(32768); size = W.DWORD(len(path))
            if not self.k.QueryFullProcessImageNameW(self.handle, 0, path, C.byref(size)):
                raise C.WinError(C.get_last_error())
            if os.path.normcase(path.value) != os.path.normcase(str(expected_path)):
                raise ValueError('Original process path no longer matches')
            self.expected_module = Path(expected_path).parent / 'prototypeenginef.dll'
        except (OSError, ValueError) as error:
            self.error = str(error)[:240]
            self.close()

    def read(self, address, size):
        if not 0 < size <= 4096:
            raise ValueError('Native read exceeds bounded block size')
        self.read_calls += 1
        block, copied = C.create_string_buffer(size), C.c_size_t()
        if not self.k.ReadProcessMemory(self.handle, address, block, size, C.byref(copied)) or copied.value != size:
            self.read_failures += 1
            raise OSError('ReadProcessMemory failed or incomplete: '+str(C.get_last_error()))
        self.read_bytes += size
        return block.raw

    def discover(self):
        if self.error or not self.handle or time.perf_counter() < self.next_discovery:
            return
        self.next_discovery = time.perf_counter() + 10
        self.discovery_attempts += 1
        snap = self.k.CreateToolhelp32Snapshot(0x18, self.pid)
        if snap == C.c_void_p(-1).value:
            raise OSError('Module snapshot unavailable: '+str(C.get_last_error()))
        found = None
        self.module_ranges = []
        try:
            entry = MODULEENTRY32W(); entry.size = C.sizeof(entry)
            more = self.k.Module32FirstW(snap, C.byref(entry))
            while more:
                if len(self.module_ranges) < 256:
                    self.module_ranges.append((entry.base, entry.base+entry.image_bytes, entry.name))
                if entry.name.lower() == 'prototypeenginef.dll':
                    found = (entry.base, entry.image_bytes, entry.path)
                more = self.k.Module32NextW(snap, C.byref(entry))
        finally:
            self.k.CloseHandle(snap)
        if found is None:
            return  # Loader may not have initialized the engine yet.
        base, image_bytes, path = found
        if (os.path.normcase(path) != os.path.normcase(str(self.expected_module)) or
                any(rva + len(code) > image_bytes for rva, code in GUARDS) or
                max(RENDER_RVA + 0x3e0, POOL_RVA + 0x8158, WORKER_ENTRY_RVA + 1) > image_bytes):
            self.error = 'Unsupported engine module identity'; return
        if any(self.read(base + rva, len(code)) != code for rva, code in GUARDS):
            self.error = 'Loaded engine code guards differ; native fields not decoded'; return
        self.base, self.verified = base, True

    def thread_record(self, address):
        data = self.read(pointer32(address, 0xb4) + 0x58, 0x5c)
        return dict(tid=u32(data, 0x34), name=ascii_name(data[0x3c:0x5c]),
                    active=bool(data[0]), object=f'{address:08x}')

    def renderer(self):
        data = self.read(self.base + RENDER_RVA, 0x3e0)
        active, tid, name = data[96], u32(data, 148), ascii_name(data[156:188])
        valid = active == 1 and tid != 0 and name == 'engine::RenderThread'
        result = dict(status='observed' if valid else 'inactive_or_initializing',
                      tid=tid if valid else None, name=name, active=bool(active),
                      initial_service_or_phase1_wait=u32(data, 988) if valid else None)
        for name, offset in (('last_producer_begin_to_phase2_complete_ms', 4),
                             ('last_producer_end_to_next_begin_ms', 352),
                             ('last_renderer_early_phase_ms', 944)):
            value = struct.unpack_from('<f', data, offset)[0]
            result[name] = round(value, 6) if valid and math.isfinite(value) and value >= 0 else None
        return result, u32(data, 956)

    def frame_queue(self, address):
        data = self.read(pointer32(address, 64), 64)
        capacity, read, write = (u32(data, i) for i in (0x2c, 0x34, 0x38))
        count, records = u32(data, 0x10), u32(data, 0x0c)
        if capacity != 8 or read >= capacity or write >= capacity or count != 2:
            raise ValueError('Frame queue initializing or inconsistent')
        result = dict(status='observed', capacity=capacity, read_index=read, write_index=write,
                      pending_tokens_snapshot=(write-read) % capacity,
                      consumer_wait_marker=bool(data[8]), processing_marker=bool(data[0x3c]),
                      critical_section=address+0x14, consumer_semaphore=u32(data, 4))
        producers = self.read(pointer32(records, count*12), count*12)
        threads = [dict(role='frame_consumer', **self.thread_record(u32(data)))]
        for index in range(count):
            thread_address = u32(producers, index*12)
            if thread_address == 0:  # Native constructor leaves unused slots unassigned.
                continue
            thread = self.thread_record(thread_address)
            threads.append(dict(role=f'frame_producer_{index}',
                                drain_semaphore=u32(producers, index*12+4),
                                drain_wait_marker=bool(producers[index*12+8]), **thread))
        result['threads'] = threads
        return result

    def pool(self):
        address = self.base + POOL_RVA
        descriptor = self.read(address + 0x8150, 8)
        workers, count = u32(descriptor), u32(descriptor, 4)
        if not 1 <= count <= 7:
            raise ValueError('Task pool inactive or unsupported worker count')
        data = self.read(pointer32(workers, count*0xc0), count*0xc0)
        records = []
        for index in range(count):
            row = data[index*0xc0:(index+1)*0xc0]
            if (u32(row, 8) != self.base + WORKER_ENTRY_RVA or
                    u32(row, 0x0c) != workers+index*0xc0 or
                    u32(row, 0xb8) != index or u32(row, 0xbc) != address):
                raise ValueError('Task worker identity/lifetime validation failed')
            records.append(dict(role=f'async_worker_{index}', tid=u32(row, 0x8c),
                                active=bool(row[0x58]), name=ascii_name(row[0x94:0xb4])))
        state = self.read(address + 0x90, 0xb4)
        if self.read(address + 0x8150, 8) != descriptor:
            raise ValueError('Task worker array changed during snapshot')
        packed = struct.unpack_from('<Q', state)[0]
        head, available, registered = packed & 0xfffffff, (packed >> 28) & 0xfffffff, packed >> 56
        cursor = u32(state, 0x98)
        if head >= 1024 or available > 1024 or cursor >= 1024:
            raise ValueError('Task queue snapshot inconsistent; not decoded')
        return dict(status='observed', worker_count=count, workers=records,
                    packed_read_state=f'{packed:016x}', head=head, published_ready=available,
                    registered_unnotified_consumers=registered, write_reservation_cursor=cursor,
                    free_reservation_balance=struct.unpack_from('<i', state, 0xa0)[0],
                    capacity_waiters=u32(state, 0xa4),
                    semaphores=dict(capacity=u32(state, 0xa8), work_available=u32(state, 0xac), crossed_capacity=u32(state, 0xb0)))

    def sample(self):
        start = time.perf_counter()
        calls, byte_count = self.read_calls, self.read_bytes
        result = dict(status='unavailable', snapshot_monotonic_ns=time.perf_counter_ns())
        try:
            if not self.base:
                self.discover()
            if not self.base:
                result['reason'] = self.error or 'engine_not_loaded_yet'
                return result
            result.update(status='layout_verified', module_base=f'{self.base:08x}')
            try:
                result['renderer'], queue = self.renderer()
            except (OSError, ValueError) as error:
                queue = 0
                result['renderer'] = dict(status='unavailable', reason=str(error)[:200])
            for name, operation in (('frame_queue', lambda: self.frame_queue(queue)), ('task_pool', self.pool)):
                try:
                    result[name] = operation()
                except (OSError, ValueError) as error:
                    result[name] = dict(status='unavailable', reason=str(error)[:200])
        except (OSError, ValueError) as error:
            result['reason'] = str(error)[:200]
        finally:
            result.update(read_calls=self.read_calls-calls, read_bytes=self.read_bytes-byte_count,
                          read_wall_ms=round((time.perf_counter()-start)*1000, 4))
        return result

    def close(self):
        if self.handle:
            self.k.CloseHandle(self.handle); self.handle = None


class GpuSample(C.Structure):
    _pack_ = 8
    _fields_ = [(name, C.c_uint32) for name in ('status', 'flags')] + [
        (name, C.c_uint64) for name in ('frame_id', 'qpc_begin', 'qpc_end', 'gpu_frequency', 'gpu_start', 'gpu_end')] + [
        (name, C.c_double) for name in ('command_span_ms', 'cpu_submit_span_ms', 'present_wall_ms', 'upload_wall_ms')] + [
        (name, C.c_uint32) for name in ('draw_calls', 'upload_calls', 'query_polls')] + [
        ('error', C.c_int32), ('upload_gpu_ms', C.c_double), ('upload_gpu_samples', C.c_uint32),
        ('upload_gpu_omitted', C.c_uint32), ('query_ready_qpc', C.c_uint64)]


class WaitSample(C.Structure):
    _pack_ = 8
    _fields_ = [(name, C.c_uint32) for name in ('generation', 'tid', 'kind', 'timeout_ms', 'result', 'last_error', 'frame_count', 'status')] + [
        ('qpc_begin', C.c_uint64), ('qpc_end', C.c_uint64), ('frames', C.c_uint32 * 12)] + [
        (name, C.c_uint32) for name in ('object', 'count', 'flags')]


class GpuSlot(C.Structure):
    _fields_ = [('sequence', C.c_int32), ('reserved', C.c_uint32), ('sample', GpuSample)]


class WaitSlot(C.Structure):
    _fields_ = [('owner_tid', C.c_int32), ('kind', C.c_uint32), ('sequence', C.c_int32),
                ('reserved', C.c_uint32), ('thread_creation', C.c_uint64), ('sample', WaitSample)]


class SharedBlock(C.Structure):
    _fields_ = [(name, C.c_uint32) for name in ('magic', 'version', 'bytes', 'pid')] + [
        ('creation', C.c_uint64), ('qpc_frequency', C.c_uint64)] + [
        (name, C.c_uint32) for name in ('engine_base', 'patch_base', 'wait_hook_mask', 'wait_slots', 'wait_drops', 'generation', 'wait_generation', 'reserved')] + [
        ('gpu', GpuSlot), ('waits', WaitSlot * 128)]


assert (C.sizeof(GpuSample), C.sizeof(WaitSample), C.sizeof(WaitSlot), C.sizeof(SharedBlock)) == (128, 112, 136, 17608)


class InstrumentedReader:
    """Read only the original PID/creation's small shared mapping. No game RPC."""
    def __init__(self, pid, creation):
        self.pid, self.creation = pid, creation
        self.handle, self.view = None, None
        self.previous_waits, self.previous_gpu = {}, 0
        self.k = C.WinDLL('kernel32', use_last_error=True)
        for name, args, result in (
                ('OpenFileMappingW', [W.DWORD, W.BOOL, W.LPCWSTR], W.HANDLE),
                ('MapViewOfFile', [W.HANDLE, W.DWORD, W.DWORD, W.DWORD, C.c_size_t], C.c_void_p),
                ('UnmapViewOfFile', [C.c_void_p], W.BOOL), ('CloseHandle', [W.HANDLE], W.BOOL)):
            fn = getattr(self.k, name); fn.argtypes = args; fn.restype = result

    def sample(self, modules=(), exiting=False, native=None):
        if not self.view:
            name = f'Local\\PrototypeModern.EngineTelemetry.{self.pid}.{self.creation}'
            self.handle = self.k.OpenFileMappingW(4, False, name)  # FILE_MAP_READ
            if not self.handle:
                return dict(status='unavailable', reason='instrumented_mapping_not_created_yet_or_capture_disabled')
            self.view = self.k.MapViewOfFile(self.handle, 4, 0, 0, C.sizeof(SharedBlock))
            if not self.view:
                self.close()
                return dict(status='unavailable', reason='instrumented_mapping_read_denied')
        # One fixed-size copy. Writer sequence is checked again per slot; skip
        # changing/odd records without spinning or locking the game.
        offsets = [SharedBlock.gpu.offset] + [SharedBlock.waits.offset + i*C.sizeof(WaitSlot) + WaitSlot.sequence.offset for i in range(128)]
        before = [C.c_int32.from_address(self.view + offset).value for offset in offsets]
        block = SharedBlock.from_buffer_copy(C.string_at(self.view, C.sizeof(SharedBlock)))
        if (block.magic != 0x31444745 or block.version != 1 or block.bytes != C.sizeof(SharedBlock) or
                block.pid != self.pid or block.creation != self.creation or not block.qpc_frequency or block.wait_slots != 128):
            return dict(status='unavailable', reason='instrumented_mapping_identity_or_schema_mismatch')
        def sequence(offset):
            return C.c_int32.from_address(self.view + offset).value
        def symbol(pc):
            for begin, end, name in modules:
                if begin <= pc < end:
                    return f'{name[:80]}+0x{pc-begin:x}'
            return f'0x{pc:08x}'
        objects = {}
        if native:
            queue = native.get('frame_queue', {})
            for name in ('critical_section', 'consumer_semaphore'):
                if queue.get(name): objects[queue[name]] = 'frame_queue.'+name
            for producer in queue.get('threads', []):
                if producer.get('drain_semaphore'): objects[producer['drain_semaphore']] = producer['role']+'.drain_semaphore'
            for name, handle in native.get('task_pool', {}).get('semaphores', {}).items():
                if handle: objects[handle] = 'task_pool.'+name
        result = dict(status='observed', qpc_frequency=block.qpc_frequency,
                      generation=block.generation, wait_hook_mask=block.wait_hook_mask,
                      wait_slots_dropped=block.wait_drops, changing_slots_skipped=0, waits=[],
                      gpu_record_sequence=block.gpu.sequence,
                      gpu_result_state='unchanged' if block.gpu.sequence else 'not_yet_published')
        gpu_seq = block.gpu.sequence
        if gpu_seq and not gpu_seq & 1 and gpu_seq != self.previous_gpu:
            if before[0] == gpu_seq == sequence(SharedBlock.gpu.offset):
                g = block.gpu.sample
                gpu = {name: getattr(g, name) for name, _ in GpuSample._fields_}
                gpu['error_hex'] = f'0x{g.error & 0xffffffff:08x}'
                for field in ('command_span_ms', 'cpu_submit_span_ms', 'present_wall_ms', 'upload_wall_ms', 'upload_gpu_ms'):
                    if not math.isfinite(gpu[field]) or gpu[field] < 0:
                        gpu[field] = None
                if not g.flags & 1: gpu['command_span_ms'] = None
                if not g.flags & 2:
                    gpu['cpu_submit_span_ms'] = gpu['present_wall_ms'] = None
                if not g.flags & 4: gpu['upload_wall_ms'] = None
                if not g.flags & 16: gpu['upload_gpu_ms'] = None
                result['gpu_timing'] = gpu
                result['gpu_result_state'] = 'new_result'
                self.previous_gpu = gpu_seq
            else:
                result['changing_slots_skipped'] += 1
        for index, slot in enumerate(block.waits):
            seq = slot.sequence
            if seq & 1:
                result['changing_slots_skipped'] += 1
                continue
            if not slot.owner_tid or not seq:
                continue
            offset = SharedBlock.waits.offset + index*C.sizeof(WaitSlot) + WaitSlot.sequence.offset
            if before[index+1] != seq or sequence(offset) != seq:
                result['changing_slots_skipped'] += 1; continue
            w = slot.sample
            identity = (w.tid, slot.thread_creation, w.kind)
            if self.previous_waits.get(identity) == seq and not (exiting and w.status == 1):
                continue
            if w.tid != slot.owner_tid or w.kind != slot.kind or w.frame_count > 12 or w.status not in (1, 2):
                result['changing_slots_skipped'] += 1; continue
            self.previous_waits[identity] = seq
            # Bounded identities: Windows may reuse slots/TIDs on later threads.
            if len(self.previous_waits) > 256:
                self.previous_waits = {identity: seq}
            item = {name: getattr(w, name) for name, _ in WaitSample._fields_ if name != 'frames'}
            item.update(thread_creation=slot.thread_creation,
                        frames=[symbol(pc) for pc in w.frames[:w.frame_count]],
                        duration_ms=(w.qpc_end-w.qpc_begin)*1000/block.qpc_frequency
                        if w.status == 2 and w.qpc_end >= w.qpc_begin else None,
                        incomplete_at_process_exit=exiting and w.status == 1)
            if w.object in objects: item['matched_nearby_snapshot_object'] = objects[w.object]
            result['waits'].append(item)
        # Preserve a hard per-record allowance as well as the whole-file cap.
        # Detailed return PCs remain bounded even with unusually long DLL names.
        selected, used = [], 0
        for row in sorted(result['waits'], key=lambda w:(w['generation'], w['duration_ms'] or 0), reverse=True):
            cost = len(json.dumps(row, ensure_ascii=False).encode('utf-8'))
            if used+cost <= 120000:
                selected.append(row); used += cost
        result['wait_detail_omitted'] = len(result['waits'])-len(selected)
        result['waits'] = selected
        return result

    def close(self):
        if self.view:
            self.k.UnmapViewOfFile(self.view); self.view = None
        if self.handle:
            self.k.CloseHandle(self.handle); self.handle = None


def roles_with_cpu(native, cpu, process, summary):
    """Join already collected counters; never call a thread-time API again."""
    roles = []
    renderer = native.get('renderer', {})
    if renderer.get('tid'):
        roles.append(dict(role='renderer', tid=renderer['tid'], active=renderer['active']))
    roles.extend(native.get('frame_queue', {}).get('threads', []))
    roles.extend(native.get('task_pool', {}).get('workers', []))
    for key, role in (('present_thread_id', 'present_log_writer'), ('startup_thread_id', 'startup_log_writer')):
        if summary.get(key):
            roles.append(dict(role=role, tid=summary[key], source='existing_native_log'))
    deltas = {row[0]: row[1:] for row in cpu['threads']}
    zero_tids = set(cpu['zero_delta_tids'])
    for tid, born in process.previous:
        if tid in zero_tids:
            deltas[tid] = (born-process.creation, 0, 0)
    result = []
    for role in roles:
        tid = role.get('tid')
        row = deltas.get(tid)
        # TID plus queried thread birth identifies the observation. Native objects
        # have no creation timestamp; joining does not prove thread lifetime across ticks.
        valid = bool(tid and row is not None and role.get('active', True))
        item = dict(role=role['role'], tid=tid,
                    born_since_process_100ns=row[0] if valid else None,
                    cpu_ms=round((row[1]+row[2])/10000, 4) if valid and row[1] is not None else None,
                    match='tid_and_observed_birth' if valid else 'not_observed_or_inactive')
        result.append(item)
    return result


class EngineCapture:
    """Independent capped file; shares process/counter/tail lifetime with the parent."""
    def __init__(self, path, process, writer, interval, started_utc):
        self.log = writer(path, limit=MAX_ENGINE_BYTES)
        self.reader = NativeReader(process.pid, process.creation, process.path)
        self.instrumented = InstrumentedReader(process.pid, process.creation)
        self.summary = dict(pid=process.pid, creation_filetime=process.creation,
                            started_utc=started_utc, format_version=1, samples=0,
                            native_verified_samples=0, unavailable_samples=0,
                            observed_task_pool_samples=0, ready_backlog_samples=0,
                            max_observed_ready=0, event_lines_omitted=0,
                            sample_wall_ms_total=0.0, sample_wall_ms_max=0.0,
                            storage_limit_bytes=MAX_ENGINE_BYTES, capabilities=CAPABILITIES,
                            native_layout_validation='loaded module path, field bounds and local instruction guards', completed=False)
        try:
            self.log.write(dict(kind='session_start', interval_s=interval, **self.summary), self.summary)
        except BaseException:
            self.close(); raise

    def fold_instrumented(self, sample):
        if sample.get('status') != 'observed':
            self.summary['instrumented_unavailable_samples'] = self.summary.get('instrumented_unavailable_samples', 0)+1
            return
        self.summary['wait_hook_mask'] = sample['wait_hook_mask']
        self.summary['wait_slots_dropped'] = sample['wait_slots_dropped']
        self.summary['wait_detail_omitted'] = self.summary.get('wait_detail_omitted', 0)+sample.get('wait_detail_omitted', 0)
        self.summary['changing_slots_skipped'] = self.summary.get('changing_slots_skipped', 0)+sample['changing_slots_skipped']
        gpu = sample.get('gpu_timing')
        if gpu:
            self.summary['last_gpu_timing'] = gpu
            key = 'valid_gpu_samples' if gpu['status'] == 1 and gpu['command_span_ms'] is not None else 'invalid_gpu_samples'
            self.summary[key] = self.summary.get(key, 0)+1
            if key == 'valid_gpu_samples':
                self.summary['max_sampled_gpu_command_ms'] = max(self.summary.get('max_sampled_gpu_command_ms', 0), gpu['command_span_ms'])
        for wait in sample['waits']:
            if wait['duration_ms'] is None:
                continue
            self.summary['completed_wait_samples'] = self.summary.get('completed_wait_samples', 0)+1
            self.summary['sampled_wait_wall_ms_sum'] = self.summary.get('sampled_wait_wall_ms_sum', 0)+wait['duration_ms']
            worst = self.summary.setdefault('longest_sampled_waits', [])
            worst.append(wait)
            worst.sort(key=lambda w:w['duration_ms'], reverse=True)
            del worst[8:]

    def events(self, lines):
        # Reuse bounded source tail; no second file read. Frame windows take priority.
        events = [line for line in lines if any(key in line for key in EVENT_MARKERS)]
        if len(events) > 48:
            frames = [line for line in events if 'metrics present ' in line]
            others = [line for line in events if 'metrics present ' not in line]
            selected = frames[-24:] + others[-max(0, 48-min(24, len(frames))):]
            self.summary['event_lines_omitted'] += len(events)-len(selected)
            events = selected
        selected, used = [], 0
        for line in sorted(events, key=lambda line:'metrics present ' not in line):
            cost = len(json.dumps(line, ensure_ascii=False).encode('utf-8'))
            if used+cost <= 24000:
                selected.append(line); used += cost
            else:
                self.summary['event_lines_omitted'] += 1
        return selected

    def sample(self, process, cpu, gpu, native, parent_summary, timestamp, monotonic_ns):
        start = time.perf_counter()
        snapshot = self.reader.sample()
        measured = self.instrumented.sample(self.reader.module_ranges, native=snapshot)
        self.fold_instrumented(measured)
        roles = roles_with_cpu(snapshot, cpu, process, parent_summary)
        self.summary['samples'] += 1
        key = 'native_verified_samples' if snapshot['status'] == 'layout_verified' else 'unavailable_samples'
        self.summary[key] += 1
        pool = snapshot.get('task_pool', {})
        if pool.get('status') == 'observed':
            self.summary['observed_task_pool_samples'] += 1
            self.summary['ready_backlog_samples'] += int(pool['published_ready'] > 0)
            self.summary['max_observed_ready'] = max(self.summary['max_observed_ready'], pool['published_ready'])
        self.summary['last_thread_roles'] = roles
        self.summary['slowest_active_windows'] = parent_summary.get('slowest_active_windows', [])
        self.summary['native_max_frame_ms'] = parent_summary.get('native_max_frame_ms')
        self.summary['source_details_omitted'] = parent_summary.get('omitted_native_details', 0)
        self.summary['source_bytes_skipped'] = parent_summary.get('skipped_native_log_bytes', 0)
        self.log.write(dict(kind='sample', pid=process.pid, sample=self.summary['samples'],
                            utc=timestamp, monotonic_ns=monotonic_ns,
                            foreground=cpu['foreground'], cpu_interval_s=cpu['interval_s'],
                            process_cpu_ms=cpu['process_cpu_ms'], inventory_age_s=cpu['inventory_age_s'],
                            screen=parent_summary.get('screen'), roles=roles, native=snapshot,
                            gpu=gpu, measured=measured, events=self.events(native)), self.summary)
        duration = (time.perf_counter()-start)*1000
        self.summary['sample_wall_ms_total'] += duration
        self.summary['sample_wall_ms_max'] = max(self.summary['sample_wall_ms_max'], duration)

    def finish(self, parent_summary, final_native, timestamp):
        measured = self.instrumented.sample(self.reader.module_ranges, exiting=True)
        self.fold_instrumented(measured)
        self.summary.update(completed=True, stopped_utc=timestamp,
                            slowest_active_windows=parent_summary.get('slowest_active_windows', []),
                            native_max_frame_ms=parent_summary.get('native_max_frame_ms'),
                            read_calls=self.reader.read_calls, read_bytes=self.reader.read_bytes,
                            read_failures=self.reader.read_failures,
                            discovery_attempts=self.reader.discovery_attempts,
                            source_details_omitted=parent_summary.get('omitted_native_details', 0),
                            source_bytes_skipped=parent_summary.get('skipped_native_log_bytes', 0),
                            detail_lines_evicted=self.log.evicted,
                            log_bytes_written_before_end=self.log.bytes_written)
        self.log.write(dict(kind='session_end', measured=measured, events=self.events(final_native), **self.summary), self.summary)

    def close(self):
        self.reader.close()
        self.instrumented.close()
