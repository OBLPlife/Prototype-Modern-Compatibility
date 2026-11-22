"""Bounded, passive CPU/GPU text telemetry. No ETW, subprocesses or game hooks.

Reads counters every five seconds, plus metrics already written by the patch.
Rolling UTF-8 JSON-lines logs: performance (900,000 bytes) and independent
engine diagnostics (1,000,000 bytes). Older details become a cumulative
checkpoint; no unbounded CSV, trace, snapshot or temporary file.
"""
import argparse
from collections import deque
import ctypes as C
from ctypes import wintypes as W
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import sys
import time

from capture_engine import EngineCapture, MAX_ENGINE_BYTES

MAX_BYTES = 900_000  # bootstrap has a separate 8,192-byte ceiling
INTERVAL = 5.0
THREAD_COLUMNS = ('tid', 'born_since_process_100ns', 'kernel_delta_100ns', 'user_delta_100ns')
NATIVE_MARKERS = (b'metrics present ', b'resources memory-valid=', b'UI action ',
                  b'input map ', b'input mouse-button ', b'input motion ', b'display ', b'window event=',
                  b'save ', b'test session local=', b'TEST ENTER ', b'TEST LEAVE ',
                  b'TEST ISSUE ', b'device recovery ', b'device lost ',
                  b'WOI ', b'woi ', b'blackfade', b'focus-return')


def utc():
    return datetime.now(timezone.utc).isoformat(timespec='milliseconds')


def encoded(value):
    return (json.dumps(value, ensure_ascii=False, separators=(',', ':'),
                       allow_nan=False) + '\n').encode('utf-8')


class TextLog:
    def __init__(self, path, limit=MAX_BYTES):
        self.path = Path(path)
        self.limit = limit
        self.rows = deque()
        self.size = 0
        self.evicted = 0
        self.bytes_written = 0
        self.path.parent.mkdir(parents=True, exist_ok=True)
        if self.path.exists():
            with self.path.open('rb') as stream:
                data = stream.read(limit + 1)
            if len(data) > limit:
                raise ValueError('Existing text log exceeds its owned byte budget')
            if data and not data.endswith(b'\n'):
                # Recover only this log's incomplete final line after interruption.
                data = data[:data.rfind(b'\n') + 1]
                self.path.write_bytes(data)
            self.rows.extend(data.splitlines(keepends=True))
            self.size = len(data)

    def write(self, record, checkpoint):
        row = encoded(record)
        if len(row) > self.limit // 4:
            raise ValueError('Oversized telemetry record')
        if self.size + len(row) > self.limit:
            while self.rows and self.size > self.limit // 2:
                self.size -= len(self.rows.popleft())
                self.evicted += 1
            note = encoded(dict(kind='checkpoint', utc=utc(),
                                evicted_detail_lines=self.evicted, **checkpoint))
            if self.size + len(note) + len(row) > self.limit:
                raise ValueError('Checkpoint exceeds telemetry budget')
            payload = b''.join(self.rows) + note + row
            # No second full-size temporary file. Only this owned text file is rewritten.
            self.path.write_bytes(payload)
            self.bytes_written += len(payload)
            self.rows.extend((note, row))
            self.size = len(payload)
        else:
            with self.path.open('ab') as stream:
                stream.write(row)
            self.rows.append(row)
            self.size += len(row)
            self.bytes_written += len(row)


class FILETIME(C.Structure):
    _fields_ = [('low', W.DWORD), ('high', W.DWORD)]

    def ticks(self):
        return (self.high << 32) | self.low


class THREADENTRY32(C.Structure):
    _fields_ = [('size', W.DWORD), ('usage', W.DWORD), ('tid', W.DWORD),
                ('pid', W.DWORD), ('base', W.LONG), ('delta', W.LONG),
                ('flags', W.DWORD)]


class ProcessCounters:
    def __init__(self, pid, creation=None, expected_path=None):
        self.pid = pid
        self.k = C.WinDLL('kernel32', use_last_error=True)
        self.u = C.WinDLL('user32', use_last_error=True)
        declarations = {
            'OpenProcess': ([W.DWORD, W.BOOL, W.DWORD], W.HANDLE),
            'OpenThread': ([W.DWORD, W.BOOL, W.DWORD], W.HANDLE),
            'CloseHandle': ([W.HANDLE], W.BOOL),
            'WaitForSingleObject': ([W.HANDLE, W.DWORD], W.DWORD),
            'CreateToolhelp32Snapshot': ([W.DWORD, W.DWORD], W.HANDLE),
            'Thread32First': ([W.HANDLE, C.POINTER(THREADENTRY32)], W.BOOL),
            'Thread32Next': ([W.HANDLE, C.POINTER(THREADENTRY32)], W.BOOL),
            'QueryFullProcessImageNameW': ([W.HANDLE, W.DWORD, W.LPWSTR,
                                           C.POINTER(W.DWORD)], W.BOOL),
        }
        for name, (args, result) in declarations.items():
            fn = getattr(self.k, name); fn.argtypes = args; fn.restype = result
        for name in ('GetProcessTimes', 'GetThreadTimes'):
            fn = getattr(self.k, name)
            fn.argtypes = [W.HANDLE] + [C.POINTER(FILETIME)] * 4
            fn.restype = W.BOOL
        self.u.GetForegroundWindow.restype = W.HWND
        self.u.GetWindowThreadProcessId.argtypes = [W.HWND, C.POINTER(W.DWORD)]
        self.handle = self.k.OpenProcess(0x1000 | 0x100000, False, pid)
        if not self.handle:
            raise C.WinError(C.get_last_error())
        try:
            self.creation, _, _ = self.times(self.handle, process=True)
            path = C.create_unicode_buffer(32768); length = W.DWORD(len(path))
            if not self.k.QueryFullProcessImageNameW(self.handle, 0, path, C.byref(length)):
                raise C.WinError(C.get_last_error())
            self.path = path.value
            if creation is not None and creation != self.creation:
                raise ValueError('Original process creation time no longer matches')
            if expected_path and os.path.normcase(self.path) != os.path.normcase(str(expected_path)):
                raise ValueError('Original process path no longer matches')
        except BaseException:
            self.close(); raise
        self.previous = {}
        self.previous_cpu = None
        self.previous_time = None
        self.thread_handles = {}
        self.next_inventory = 0.0
        self.last_inventory = 0.0

    def times(self, handle, process=False):
        start, end, kernel, user = FILETIME(), FILETIME(), FILETIME(), FILETIME()
        fn = self.k.GetProcessTimes if process else self.k.GetThreadTimes
        if not fn(handle, C.byref(start), C.byref(end), C.byref(kernel), C.byref(user)):
            raise C.WinError(C.get_last_error())
        # Both components already arrive in the same existing API call. Keep
        # integer ticks rather than discarding the split or querying again.
        return start.ticks(), kernel.ticks(), user.ticks()

    def exited(self, milliseconds=0):
        result = self.k.WaitForSingleObject(self.handle, milliseconds)
        if result == 0xFFFFFFFF:
            raise C.WinError(C.get_last_error())
        return result == 0

    def refresh_threads(self, now):
        # System-wide thread enumeration was measured at ~61 ms here. Do it
        # at startup, once after engine initialization, then only once/minute.
        # Five-second samples read cached query-only handles instead.
        snap = self.k.CreateToolhelp32Snapshot(4, 0)
        if snap == C.c_void_p(-1).value:
            raise C.WinError(C.get_last_error())
        fresh = {}
        try:
            entry = THREADENTRY32(); entry.size = C.sizeof(entry)
            valid = self.k.Thread32First(snap, C.byref(entry))
            while valid:
                if entry.pid == self.pid:
                    thread = self.thread_handles.pop(entry.tid, None)
                    if thread is None:
                        thread = self.k.OpenThread(0x0800, False, entry.tid)
                    if thread:
                        fresh[entry.tid] = thread
                valid = self.k.Thread32Next(snap, C.byref(entry))
        finally:
            self.k.CloseHandle(snap)
        for handle in self.thread_handles.values():
            self.k.CloseHandle(handle)
        self.thread_handles = fresh
        self.next_inventory = now + (10 if not self.last_inventory else 60)
        self.last_inventory = now

    def sample(self):
        now = time.perf_counter()
        _, kernel, user = self.times(self.handle, process=True)
        dt = now - self.previous_time if self.previous_time is not None else None
        dk = kernel-self.previous_cpu[0] if self.previous_cpu is not None else None
        du = user-self.previous_cpu[1] if self.previous_cpu is not None else None
        delta = (dk+du)/10_000 if dk is not None else None
        fg = W.DWORD()
        self.u.GetWindowThreadProcessId(self.u.GetForegroundWindow(), C.byref(fg))
        inventory_ms = 0.0
        if now >= self.next_inventory:
            start = time.perf_counter()
            self.refresh_threads(now)
            inventory_ms = (time.perf_counter()-start)*1000
        threads, idle_tids, fresh, failures = [], [], {}, 0
        for tid, handle in self.thread_handles.items():
            try:
                born, tk, tu = self.times(handle)
                key = (tid, born)
                previous = self.previous.get(key)
                fresh[key] = (tk, tu)
                # All queried threads, including zero deltas. Compact rows cost
                # less formatting than sorting/dictionaries/percentages per thread.
                # A missing baseline stays null, never an invented zero.
                if previous and (tk, tu) == previous:
                    idle_tids.append(tid)
                else:
                    threads.append((tid, born-self.creation,
                                    tk-previous[0] if previous else None,
                                    tu-previous[1] if previous else None))
            except OSError:
                failures += 1
        self.previous, self.previous_cpu, self.previous_time = fresh, (kernel, user), now
        return dict(interval_s=round(dt, 6) if dt else None, foreground=fg.value == self.pid,
                    process_cpu_ms=round(delta, 3) if delta is not None else None,
                    process_kernel_ms=round(dk/10_000, 3) if dk is not None else None,
                    process_user_ms=round(du/10_000, 3) if du is not None else None,
                    one_cpu_pct=round(delta/dt/10, 2) if dt else None,
                    observed_threads=len(fresh), inventory_age_s=round(now-self.last_inventory, 3),
                    inventory_refresh_ms=round(inventory_ms, 3),
                    thread_read_failures=failures, threads=threads, zero_delta_tids=idle_tids)

    def close(self):
        for handle in getattr(self, 'thread_handles', {}).values():
            self.k.CloseHandle(handle)
        self.thread_handles = {}
        if self.handle:
            self.k.CloseHandle(self.handle); self.handle = None


class Utilization(C.Structure):
    _fields_ = [('gpu', C.c_uint), ('memory', C.c_uint)]


class Memory(C.Structure):
    _fields_ = [('total', C.c_ulonglong), ('free', C.c_ulonglong), ('used', C.c_ulonglong)]


class NvidiaCounters:
    """Read-only NVML queries; whole-device samples, not per-frame GPU timings."""
    def __init__(self):
        self.lib, self.devices, self.error = None, [], None
        try:
            lib = C.CDLL(str(Path(os.environ['WINDIR']) / 'System32/nvml.dll'))
            if lib.nvmlInit_v2() != 0:
                raise OSError('NVML initialization failed')
            self.lib = lib
            count = C.c_uint()
            if lib.nvmlDeviceGetCount_v2(C.byref(count)) != 0:
                raise OSError('NVML device enumeration failed')
            for index in range(count.value):
                handle = C.c_void_p(); name = C.create_string_buffer(128)
                if lib.nvmlDeviceGetHandleByIndex_v2(C.c_uint(index), C.byref(handle)) == 0:
                    lib.nvmlDeviceGetName(handle, name, C.c_uint(len(name)))
                    self.devices.append((index, handle, name.value.decode('utf-8', 'replace')))
        except (OSError, AttributeError, KeyError) as error:
            self.error = str(error)[:240]

    def sample(self):
        result = []
        for index, handle, name in self.devices:
            item = dict(index=index, name=name)
            util, memory = Utilization(), Memory()
            if self.lib.nvmlDeviceGetUtilizationRates(handle, C.byref(util)) == 0:
                item.update(gpu_pct=util.gpu, memory_controller_pct=util.memory)
            if self.lib.nvmlDeviceGetMemoryInfo(handle, C.byref(memory)) == 0:
                item.update(vram_used_mib=round(memory.used/1048576, 1), vram_total_mib=round(memory.total/1048576, 1))
            for key, function, extra, scale in (
                ('power_w', 'nvmlDeviceGetPowerUsage', (), 1000),
                ('temperature_c', 'nvmlDeviceGetTemperature', (C.c_uint(0),), 1),
                ('graphics_mhz', 'nvmlDeviceGetClockInfo', (C.c_uint(0),), 1),
                ('memory_mhz', 'nvmlDeviceGetClockInfo', (C.c_uint(2),), 1),
            ):
                value = C.c_uint()
                if getattr(self.lib, function)(handle, *extra, C.byref(value)) == 0:
                    item[key] = round(value.value/scale, 2)
            result.append(item)
        return dict(devices=result, unavailable=self.error)

    def close(self):
        if self.lib:
            self.lib.nvmlShutdown(); self.lib = None


class GameLogTail:
    def __init__(self, path, pid):
        self.path, self.pid = Path(path), pid
        self.position, self.pending, self.matched = 0, b'', False
        self.skipped = 0
        self.omitted_details = 0
        self.motion_records = 0
        self.prologue = None

    def read(self):
        if not self.path.exists():
            return []
        size = self.path.stat().st_size
        if size < self.position:
            self.position, self.pending, self.matched = 0, b'', False
            self.prologue = None
        with self.path.open('rb') as stream:
            stream.seek(self.position)
            data = stream.read(262144)
            self.position += len(data)
        lines = (self.pending + data).split(b'\n')
        self.pending = lines.pop()[-4096:]
        result, detail_bytes = [], 0
        for raw in lines:
            # Most original lines are motion polling. Filter their ASCII tags
            # before decoding, freeing work for the additional useful fields.
            if b'input state ' in raw:
                if (b':delta=' in raw and b':down=' not in raw and
                        b':up=' not in raw and b':value=' not in raw):
                    if self.matched:
                        self.motion_records += 1
                    continue  # legacy motion-only rows; preserve both edges and absolute axes
            elif b'Prototype Modern Compatibility Patch ' in raw:
                self.prologue = raw.decode('gb18030', 'replace').strip()[:1200]
                continue
            elif not any(key in raw for key in NATIVE_MARKERS):
                continue
            line = raw.decode('gb18030', 'replace').strip()
            if 'test session local=' in line:
                self.matched = bool(re.search(r'\bpid='+str(self.pid)+r'\b', line))
                if self.matched and self.prologue:
                    result.append(self.prologue)
                    detail_bytes += len(encoded(self.prologue))
                    self.prologue = None
            if self.matched:
                line = line[:1200]
                # A noisy event burst must not abort capture with an oversized
                # JSON record. Report omissions explicitly; the raw game log is
                # unchanged. Normal frame/resource windows are far below this cap.
                size_hint = len(encoded(line))
                if detail_bytes + size_hint > 65_536 and 'metrics present ' in line:
                    # Frame windows take precedence over verbose UI events.
                    for index in range(len(result)-1, -1, -1):
                        if 'metrics present ' not in result[index]:
                            detail_bytes -= len(encoded(result.pop(index)))
                            self.omitted_details += 1
                            if detail_bytes + size_hint <= 65_536:
                                break
                if detail_bytes + size_hint <= 65_536:
                    result.append(line)
                    detail_bytes += size_hint
                else:
                    self.omitted_details += 1
        # A burst cannot turn this observer into an unbounded log reader.
        if size > self.position + 262144:
            self.skipped += size-self.position
            self.position, self.pending = size, b''
        return result


def fold_native(summary, lines):
    for line in lines:
        if 'input motion ' in line:
            fields=dict(re.findall(r'([\w-]+)=(-?\d+)',line))
            summary['native_motion_windows']=summary.get('native_motion_windows',0)+1
            for name,field in [('samples','samples'),('dx','dx'),('dy','dy'),('wheel','wheel')]:
                key='native_motion_'+name
                summary[key]=summary.get(key,0)+int(fields.get(field,0))
            continue
        match = re.search(r'intervals=(\d+) avg-fps=([\d.]+).*?max-ms=([\d.]+)', line)
        if match:
            count, fps, maximum = int(match[1]), float(match[2]), float(match[3])
            summary['native_frames'] += count
            if fps > 0:
                summary['native_interval_s'] += count/fps
            summary['native_max_frame_ms'] = max(summary['native_max_frame_ms'], maximum)
            fields = dict(re.findall(r'([\w-]+)=([\d.]+)', line))
            dispatch = int(fields.get('render-dispatch', 0))
            simulated = int(fields.get('sim-callbacks', 0))
            if fps > 0 and fields.get('foreground') == '1' and dispatch and simulated >= .98*dispatch:
                summary['active_frames'] = summary.get('active_frames', 0)+count
                summary['active_interval_s'] = summary.get('active_interval_s', 0)+count/fps
                prefix = re.match(r'\[(\d+) ms tid=(\d+)\]', line)
                window = dict(end_ms=int(prefix[1]) if prefix else None,
                              intervals=count, fps=fps, max_ms=maximum,
                              p95_ms=float(fields['p95-ms']) if 'p95-ms' in fields else None,
                              p99_ms=float(fields['p99-ms']) if 'p99-ms' in fields else None,
                              screen=summary.get('screen'))
                worst = summary.setdefault('slowest_active_windows', [])
                worst.append(window)
                worst.sort(key=lambda w: w['fps'])
                del worst[8:]
            prefix = re.match(r'\[\d+ ms tid=(\d+)\]', line)
            if prefix:
                summary['present_thread_id'] = int(prefix[1])
        elif 'resources memory-valid=1' in line:
            match = re.search(r'private-bytes=(\d+)', line)
            if match:
                summary['peak_observed_private_bytes'] = max(
                    summary.get('peak_observed_private_bytes', 0), int(match[1]))
        elif 'PM_ReportAction | screen |' in line:
            screen = line.split('PM_ReportAction | screen |', 1)[1].strip()
            if screen != 'undefined':
                summary['screen'] = screen
        elif 'test session local=' in line:
            prefix = re.match(r'\[\d+ ms tid=(\d+)\]', line)
            if prefix:
                summary['startup_thread_id'] = int(prefix[1])
        elif 'TEST ISSUE ' in line:
            summary['user_issue_marks'] = summary.get('user_issue_marks', 0)+1


def run(pid, creation, game_directory, output, interval=INTERVAL):
    started_wall, started_cpu = time.perf_counter(), time.process_time()
    process = ProcessCounters(pid, creation, Path(game_directory)/'prototypef.exe')
    process.k.GetCurrentProcess.restype = W.HANDLE
    process.k.SetPriorityClass.argtypes = [W.HANDLE, W.DWORD]
    lowered_priority = bool(process.k.SetPriorityClass(process.k.GetCurrentProcess(), 0x4000))
    gpu, engine = None, None
    process.k.CreateMutexW.argtypes = [C.c_void_p, W.BOOL, W.LPCWSTR]
    process.k.CreateMutexW.restype = W.HANDLE
    process.k.ReleaseMutex.argtypes = [W.HANDLE]
    mutex = process.k.CreateMutexW(None, True, 'Local\\PrototypeModernPassiveTextCapture')
    if not mutex or C.get_last_error() == 183:
        if mutex:
            process.k.CloseHandle(mutex)
        process.close()
        raise RuntimeError('Another passive text collector is already active')
    summary = dict(pid=pid, started_utc=utc(), format_version=2,
                   creation_filetime=process.creation, thread_columns=THREAD_COLUMNS,
                   samples=0, native_frames=0,
                   native_interval_s=0.0, native_max_frame_ms=0.0,
                   cpu_sample_ms_total=0.0, gpu_sample_ms_total=0.0,
                   sample_ms_max=0.0, completed=False)
    try:
        log = TextLog(output)
        gpu = NvidiaCounters()
        tail = GameLogTail(Path(game_directory)/'PrototypeModern.log', pid)
        log.write(dict(kind='session_start', **summary,
                       interval_s=interval, storage_limit_bytes=MAX_BYTES,
                       observer_below_normal_priority=lowered_priority,
                       gpu_scope='whole device; no per-frame GPU Busy/Wait or ETW stacks',
                       cpu_scope='all queried threads; zero_delta_tids have zero CPU delta; 100ns ticks',
                       motion_scope='native_motion_* sums source aggregates; aggregated_motion_records counts filtered legacy motion-only lines, never button releases',
                       frames_source='existing patch metrics; no new per-frame instrumentation',
                       separate_engine_log='engine.log', engine_storage_limit_bytes=MAX_ENGINE_BYTES,
                       performance_validity='PENDING_GAME_AB; observer self-cost is not zero-impact proof'), summary)
        try:
            engine = EngineCapture(Path(output).with_name('engine.log'), process, TextLog,
                                   interval, summary['started_utc'])
        except (OSError, ValueError) as error:
            log.write(dict(kind='engine_capture_error', pid=pid, utc=utc(), message=str(error)[:500]), summary)
        while not process.exited():
            begin = time.perf_counter()
            cpu_data = process.sample()
            after_cpu = time.perf_counter()
            gpu_data = gpu.sample()
            after_gpu = time.perf_counter()
            native = tail.read()
            fold_native(summary, native)
            summary['aggregated_motion_records'] = tail.motion_records
            summary['omitted_native_details'] = tail.omitted_details
            summary['skipped_native_log_bytes'] = tail.skipped
            summary['samples'] += 1
            summary['cpu_sample_ms_total'] += (after_cpu-begin)*1000
            summary['gpu_sample_ms_total'] += (after_gpu-after_cpu)*1000
            timestamp, monotonic_ns = utc(), time.perf_counter_ns()
            if engine:
                try:
                    engine.sample(process, cpu_data, gpu_data, native, summary, timestamp, monotonic_ns)
                except (OSError, ValueError) as error:
                    engine.close(); engine = None
                    log.write(dict(kind='engine_capture_error', pid=pid, utc=timestamp, message=str(error)[:500]), summary)
            summary['sample_ms_max'] = max(summary['sample_ms_max'], (time.perf_counter()-begin)*1000)
            log.write(dict(kind='sample', pid=pid, sample=summary['samples'], utc=timestamp, monotonic_ns=monotonic_ns,
                           cpu=cpu_data, gpu=gpu_data, native=native,
                           observer_cpu_s=round(time.process_time()-started_cpu, 6),
                           sample_wall_ms=round((time.perf_counter()-begin)*1000, 3)), summary)
            if process.exited(max(0, round((interval-(time.perf_counter()-begin))*1000))):
                break
        # Final drain after process exit, without keeping the game alive.
        final_native = tail.read()
        fold_native(summary, final_native)
        summary['aggregated_motion_records'] = tail.motion_records
        summary['omitted_native_details'] = tail.omitted_details
        summary['skipped_native_log_bytes'] = tail.skipped
        if final_native:
            log.write(dict(kind='final_native', pid=pid, utc=utc(), native=final_native), summary)
        if engine:
            try:
                engine.finish(summary, final_native, utc())
            except (OSError, ValueError) as error:
                log.write(dict(kind='engine_capture_error', pid=pid, utc=utc(), message=str(error)[:500]), summary)
        elapsed = time.perf_counter()-started_wall
        summary.update(completed=True, stopped_utc=utc(), elapsed_s=round(elapsed, 3),
                       observer_cpu_s=round(time.process_time()-started_cpu, 6),
                       observer_one_cpu_pct=round(100*(time.process_time()-started_cpu)/elapsed, 4),
                       log_bytes_written=log.bytes_written, skipped_native_log_bytes=tail.skipped,
                       omitted_native_details=tail.omitted_details,
                       detail_lines_evicted=log.evicted)
        log.write(dict(kind='session_end', **summary), summary)
    except BaseException as error:
        if 'log' in locals():
            log.write(dict(kind='capture_error', pid=pid, utc=utc(), message=str(error)[:500]), summary)
        raise
    finally:
        process.k.ReleaseMutex(mutex)
        process.k.CloseHandle(mutex)
        process.close()
        if engine:
            engine.close()
        if gpu:
            gpu.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--creation', type=int, required=True)
    parser.add_argument('--game', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    run(args.pid, args.creation, args.game, args.output)


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(str(error)[:500], file=sys.stderr)
        sys.exit(1)
