using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Web.Script.Serialization;

namespace PrototypeModern {
    public class FunctionProfile {
        public string name, prefix, sha256;
        public int rva, length;
        public bool rvaLocked;
    }
    public class BuildProfile {
        public int schema, imageBase;
        public string version;
        public FunctionProfile[] functions;
        public ResourceSpan[] spans;
        public int[] traces;
    }
    public class ResourceSpan { public int rva,length; public string sha256; }
    public class Section {
        public int Header, Rva, VirtualSize, Raw, RawSize, Flags;
        public string Name;
    }
    public sealed class PeImage {
        public byte[] Bytes;
        public int Pe, Optional, Table, Count, ImageBase, FileAlignment, SectionAlignment;
        public List<Section> Sections = new List<Section>();
        public int U16(int at) { Bounds(at, 2); return BitConverter.ToUInt16(Bytes, at); }
        public int I32(int at) { Bounds(at, 4); return BitConverter.ToInt32(Bytes, at); }
        public void Bounds(int at, int count) {
            if (at < 0 || count < 0 || (long)at + count > Bytes.Length) throw new InvalidDataException(InstallerText.PeBounds);
        }
        public PeImage(byte[] data) {
            Bytes = data;
            if (U16(0) != 0x5a4d) throw new InvalidDataException(InstallerText.Mz);
            Pe = I32(0x3c);
            if (I32(Pe) != 0x4550 || U16(Pe + 4) != 0x14c) throw new InvalidDataException(InstallerText.PeX86);
            Count = U16(Pe + 6); Optional = Pe + 24;
            if (U16(Optional) != 0x10b || U16(Pe + 20) < 224) throw new InvalidDataException(InstallerText.PeLayout);
            ImageBase = I32(Optional + 28); SectionAlignment = I32(Optional + 32); FileAlignment = I32(Optional + 36);
            if (FileAlignment < 512 || (FileAlignment & (FileAlignment - 1)) != 0 ||
                SectionAlignment < FileAlignment || (SectionAlignment & (SectionAlignment - 1)) != 0)
                throw new InvalidDataException(InstallerText.PeAlignment);
            Table = Optional + U16(Pe + 20);
            for (int i = 0; i < Count; ++i) {
                int h = Table + i * 40; Bounds(h, 40);
                var s = new Section {Header=h, Name=Encoding.ASCII.GetString(Bytes,h,8).TrimEnd('\0'),
                    VirtualSize=I32(h+8), Rva=I32(h+12), RawSize=I32(h+16), Raw=I32(h+20), Flags=I32(h+36)};
                Bounds(s.Raw, s.RawSize); Sections.Add(s);
            }
        }
        public int Offset(int rva, int length) {
            foreach (var s in Sections)
                if (rva >= s.Rva && (long)rva + length <= (long)s.Rva + s.RawSize) {
                    int at = checked(s.Raw + rva - s.Rva); Bounds(at, length); return at;
                }
            throw new InvalidDataException(InstallerText.SectionRange);
        }
        public string CString(int rva) {
            int at = Offset(rva, 1), end = at;
            while (end < Bytes.Length && Bytes[end] != 0 && end - at < 4096) ++end;
            if (end == Bytes.Length || end - at >= 4096) throw new InvalidDataException(InstallerText.PeString);
            return Encoding.ASCII.GetString(Bytes, at, end-at);
        }
        public int Export(string name) {
            int directory = I32(Optional+96);
            int at = Offset(directory,40), count = I32(at+24);
            int functions = Offset(I32(at+28),4), names = Offset(I32(at+32),checked(count*4));
            int ordinals = Offset(I32(at+36),checked(count*2));
            for(int i=0;i<count;++i)
                if(CString(I32(names+i*4))==name) return I32(functions+U16(ordinals+i*2)*4);
            throw new InvalidDataException(InstallerText.GameExport+name);
        }
        public int Import(string dll, string name) {
            int at = Offset(I32(Optional+104),20);
            for(int d=0; d<256; ++d,at+=20) {
                int original=I32(at), module=I32(at+12), first=I32(at+16);
                if(module==0) break;
                if(!String.Equals(CString(module),dll,StringComparison.OrdinalIgnoreCase)) continue;
                int thunk=Offset(original==0?first:original,4);
                for(int i=0;i<4096;++i) {
                    int item=I32(thunk+i*4); if(item==0) break;
                    if(item>0 && CString(item+2)==name) return first+i*4;
                }
            }
            throw new InvalidDataException(InstallerText.SystemImport+dll+"!"+name);
        }
        public int ImportOrdinal(string dll,int ordinal) {
            int at=Offset(I32(Optional+104),20);
            for(int d=0;d<256;++d,at+=20) {
                int original=I32(at),module=I32(at+12),first=I32(at+16);
                if(module==0)break;
                if(!String.Equals(CString(module),dll,StringComparison.OrdinalIgnoreCase))continue;
                int thunk=Offset(original==0?first:original,4);
                for(int i=0;i<4096;++i) {
                    int item=I32(thunk+i*4);if(item==0)break;
                    if(unchecked((uint)item)==(0x80000000u|(uint)ordinal))return first+i*4;
                }
            }
            throw new InvalidDataException(InstallerText.OrdinalImport+dll+"!#"+ordinal);
        }
    }

    // Tiny purpose-specific x86 emitter. The payload is original code, not a
    // copied loader. Every absolute value below is an RVA or a stack offset.
    internal sealed class Bootstrap {
        readonly List<byte> b = new List<byte>();
        readonly Dictionary<string,int> labels = new Dictionary<string,int>();
        readonly List<Tuple<int,string,bool>> fixes = new List<Tuple<int,string,bool>>();
        readonly int rva;
        void E(params byte[] values) { b.AddRange(values); }
        void D(int value) { E(BitConverter.GetBytes(value)); }
        void Mark(string name) { labels.Add(name,b.Count); }
        void Ref(string name, bool relative) { fixes.Add(Tuple.Create(b.Count,name,relative)); D(0); }
        void Jz(string name) { E(0x0f,0x84); Ref(name,true); }
        void LeaEax(string name) { E(0x8d,0x83); Ref(name,false); }
        void CallImport(int slot) { E(0xff,0x93); D(slot); }
        void GetProc(PeImage pe, string name) {
            LeaEax(name); E(0x50,0x56); CallImport(pe.Import("KERNEL32.dll","GetProcAddress"));
            E(0x85,0xc0); Jz("fail");
        }
        public Bootstrap(int sectionRva) { rva=sectionRva; }
        public byte[] Build(PeImage pe, int init, int input, int memory, int[] traceOffsets) {
            // Preserve the incoming ABI, including x87/SSE and the direction flag.
            E(0x9c,0x60,0x8b,0xec,0x81,0xec); D(0x700);
            E(0x83,0xe4,0xf0,0x0f,0xae,0x04,0x24,0xfc);
            E(0xe8); D(0); int pc=rva+b.Count; E(0x5b,0x81,0xeb); D(pc);
            LeaEax("kernel"); E(0x50); CallImport(pe.Import("KERNEL32.dll","GetModuleHandleA"));
            E(0x85,0xc0); Jz("fail"); E(0x8b,0xf0); // ESI=kernel32
            GetProc(pe,"getfilename"); E(0x8b,0xf8); // EDI=GetModuleFileNameW
            GetProc(pe,"loadlibrary"); E(0x89,0x84,0x24); D(0x200);
            E(0x8d,0x84,0x24); D(0x220); E(0x68); D(260); E(0x50,0x53,0xff,0xd7);
            E(0x85,0xc0); Jz("fail"); E(0x3d); D(260); E(0x0f,0x83); Ref("fail",true);
            E(0x8d,0x94,0x24); D(0x220); E(0x8d,0x3c,0x42); // EDI=end; EDX=start
            Mark("findslash"); E(0x3b,0xfa); Jz("fail"); E(0x83,0xef,0x02,0x66,0x83,0x3f,0x5c);
            E(0x0f,0x85); Ref("findslash",true); E(0x83,0xc7,0x02);
            E(0x8d,0xb3); Ref("module",false); E(0xb9); D("PrototypeModern.dll".Length+1); E(0xf3,0x66,0xa5);
            E(0x8b,0x84,0x24); D(0x200); E(0x8d,0x8c,0x24); D(0x220); E(0x51,0xff,0xd0);
            E(0x85,0xc0); Jz("fail"); E(0x8b,0xf0);
            GetProc(pe,"initialize");
            E(0xff,0x75,0x28); // original EngineInitialize argument at saved ESP+40
            E(0x8d,0x8b); Ref("profile",false); E(0x51,0x53,0xff,0xd0,0x85,0xc0); Jz("fail");
            E(0xe9); Ref("resume",true);
            Mark("fail"); E(0x6a,0x10); LeaEax("title"); E(0x50); LeaEax("error"); E(0x50,0x6a,0x00);
            CallImport(pe.Import("USER32.dll","MessageBoxA"));
            Mark("resume"); E(0x0f,0xae,0x0c,0x24,0x8b,0xe5,0x61,0x9d);
            E(pe.Bytes.Skip(pe.Offset(init,6)).Take(6).ToArray());
            E(0xe9); D(init+6-(rva+b.Count+4));
            Mark("kernel"); E(Encoding.ASCII.GetBytes("kernel32.dll\0"));
            Mark("getfilename"); E(Encoding.ASCII.GetBytes("GetModuleFileNameW\0"));
            Mark("loadlibrary"); E(Encoding.ASCII.GetBytes("LoadLibraryW\0"));
            Mark("initialize"); E(Encoding.ASCII.GetBytes("PM_Initialize\0"));
            Mark("module"); E(Encoding.Unicode.GetBytes("PrototypeModern.dll\0"));
            Mark("title"); E(Encoding.ASCII.GetBytes("PrototypeModern\0"));
            Mark("error"); E(Encoding.ASCII.GetBytes("Startup patch could not initialize. Restore or reinstall the patch. See PrototypeModern.log if present.\0"));
            while((b.Count&3)!=0) E(0);
            Mark("profile"); D(0x314d4350); D(2); D(input); D(memory);
            D(pe.Import("d3d9.dll","Direct3DCreate9")); D(traceOffsets.Length);
            foreach(int offset in traceOffsets) {
                int site=init+offset, raw=pe.Offset(site,5);
                if(pe.Bytes[raw]!=0xe8) throw new InvalidDataException(InstallerText.TraceSite);
                D(site); D(checked(site+5+pe.I32(raw+1)));
            }
            foreach(var f in fixes) {
                int value=f.Item3 ? labels[f.Item2]-f.Item1-4 : rva+labels[f.Item2];
                byte[] bytes=BitConverter.GetBytes(value);
                for(int i=0;i<4;++i) b[f.Item1+i]=bytes[i];
            }
            return b.ToArray();
        }
    }

    public static class PatchImage {
        public static string Hash(byte[] bytes) {return Hash(bytes,0,bytes.Length);}
        static string Hash(byte[] bytes,int offset,int count) {
            using(var hash=SHA256.Create())return BitConverter.ToString(hash.ComputeHash(bytes,offset,count)).Replace("-","").ToLowerInvariant();
        }
        public static byte[] Hex(string text) { return Enumerable.Range(0,text.Length/2).Select(i=>Convert.ToByte(text.Substring(i*2,2),16)).ToArray(); }
        public static int Align(int value,int alignment) { return checked((value+alignment-1)/alignment*alignment); }
        static void Put(byte[] bytes,int at,int value) { Buffer.BlockCopy(BitConverter.GetBytes(value),0,bytes,at,4); }
        static int FindFunction(PeImage pe,FunctionProfile function) {
            byte[] prefix=Hex(function.prefix); var matches=new List<int>();
            if(pe.Sections.Any(s=>(s.Flags&0x20000000)!=0&&s.Rva<=function.rva&&
                    (long)function.rva+function.length<=(long)s.Rva+s.RawSize)){
                int expected=pe.Offset(function.rva,function.length);
                if(Hash(pe.Bytes,expected,function.length)==function.sha256)return function.rva;
            }
            foreach(var s in pe.Sections.Where(x=>(x.Flags&0x20000000)!=0)) {
                for(int i=s.Raw;i<=s.Raw+s.RawSize-prefix.Length;++i) {
                    if(pe.Bytes[i]!=prefix[0]) continue;
                    bool same=true;
                    for(int j=1;j<prefix.Length;++j) if(pe.Bytes[i+j]!=prefix[j]) {same=false;break;}
                    if(!same) continue;
                    if((long)i+function.length>s.Raw+s.RawSize) continue;
                    if(Hash(pe.Bytes,i,function.length)==function.sha256) matches.Add(s.Rva+i-s.Raw);
                }
            }
            if(matches.Count!=1) throw new InvalidDataException(InstallerText.FunctionSignature+function.name+InstallerText.UnknownBuild);
            if(function.rvaLocked&&matches[0]!=function.rva)throw new InvalidDataException(InstallerText.FunctionLayout+function.name);
            return matches[0];
        }
        public static bool ResourcesDiffer(byte[] image,BuildProfile profile) {
            var pe=new PeImage(image);
            foreach(var f in profile.functions) {
                try {
                    int at=pe.Offset(f.rva,f.length);
                    if(Hash(image,at,f.length)!=f.sha256)return true;
                } catch(InvalidDataException){return true;}
            }
            if(profile.spans!=null)foreach(var span in profile.spans) {
                try {
                    int at=pe.Offset(span.rva,span.length);
                    if(Hash(image,at,span.length)!=span.sha256)return true;
                }catch(InvalidDataException){return true;}
            }
            return false;
        }
        public static byte[] CreateVariant(byte[] original,BuildProfile profile,out string description) {
            var pe=new PeImage(original);
            int init=pe.Export("EngineInitialize");
            int entry=pe.Offset(init,6);
            // The bootstrap replays one six-byte, position-independent SUB ESP, imm32.
            if(!pe.Sections.Any(s=>(s.Flags&0x20000000)!=0&&init>=s.Rva&&(long)init+6<=(long)s.Rva+s.RawSize)||
                pe.Bytes[entry]!=0x81||pe.Bytes[entry+1]!=0xec)
                throw new InvalidDataException(InstallerText.InitExport);
            var locations=new Dictionary<string,int>();
            locations.Add("initialize",init);
            foreach(var f in profile.functions.Where(f=>f.name=="input"||f.name=="memory")) {
                int location;
                try{location=FindFunction(pe,f);}
                catch(InvalidDataException) {
                    // The native module checks these displaced instruction bytes again at runtime.
                    byte[] prefix=Hex(f.prefix).Take(f.name=="input"?6:5).ToArray();
                    int at=pe.Offset(f.rva,prefix.Length);
                    if(!pe.Bytes.Skip(at).Take(prefix.Length).SequenceEqual(prefix))throw;
                    location=f.rva;
                }
                locations.Add(f.name,location);
            }
            foreach(var f in profile.functions.Where(f=>f.rvaLocked))pe.Offset(f.rva,f.length);
            var calls=profile.traces.Where(offset=>pe.Bytes[pe.Offset(checked(init+offset),5)]==0xe8).ToArray();
            return BuildImage(original,pe,profile,locations,calls,out description);
        }
        internal static byte[] BeforeOwnBootstrap(byte[] original,byte[] current,BuildProfile profile) {
            string description;
            var patched=CreateVariant(original,profile,out description);
            if(patched.Length!=current.Length)return null;
            for(int i=original.Length;i<patched.Length;i++)if(current[i]!=patched[i])return null;
            var result=new byte[original.Length];
            Buffer.BlockCopy(current,0,result,0,result.Length);
            for(int i=0;i<result.Length;i++) {
                if(original[i]==patched[i])continue;
                if(current[i]!=patched[i])return null;
                result[i]=original[i];
            }
            return result;
        }
        public static byte[] Create(byte[] original,BuildProfile profile,out string description) {
            var pe=new PeImage(original);
            var functions=profile.functions.ToDictionary(x=>x.name,x=>FindFunction(pe,x));
            return BuildImage(original,pe,profile,functions,profile.traces,out description);
        }
        static byte[] BuildImage(byte[] original,PeImage pe,BuildProfile profile,Dictionary<string,int> functions,int[] traces,out string description) {
            if(pe.ImageBase!=profile.imageBase) throw new InvalidDataException(InstallerText.ImageLayout);
            if(pe.Sections.Any(x=>x.Name==".pmc")) throw new InvalidDataException(InstallerText.SectionExists);
            if(pe.I32(pe.Optional+96+4*8)!=0) throw new InvalidDataException(InstallerText.EngineSigned);
            int init=functions["initialize"];
            if(pe.Export("EngineInitialize")!=init) throw new InvalidDataException(InstallerText.InitExport);
            int h=pe.Table+pe.Count*40;
            if(h+40>pe.Sections.Min(x=>x.Raw) || h+40>pe.I32(pe.Optional+60)) throw new InvalidDataException(InstallerText.HeaderSpace);
            for(int i=h;i<h+40;++i) if(original[i]!=0) throw new InvalidDataException(InstallerText.SectionOccupied);
            int rva=Align(pe.Sections.Max(x=>checked(x.Rva+Math.Max(x.VirtualSize,x.RawSize))),pe.SectionAlignment);
            int raw=Align(original.Length,pe.FileAlignment);
            byte[] payload=new Bootstrap(rva).Build(pe,init,functions["input"],functions["memory"],traces);
            int rawSize=Align(payload.Length,pe.FileAlignment);
            var result=new byte[checked(raw+rawSize)]; Buffer.BlockCopy(original,0,result,0,original.Length);
            Buffer.BlockCopy(payload,0,result,raw,payload.Length);
            Buffer.BlockCopy(Encoding.ASCII.GetBytes(".pmc"),0,result,h,4);
            Put(result,h+8,payload.Length); Put(result,h+12,rva); Put(result,h+16,rawSize); Put(result,h+20,raw);
            Put(result,h+36,0x60000020); // read/execute only; no permanent RWX section
            byte[] count=BitConverter.GetBytes((ushort)(pe.Count+1)); Buffer.BlockCopy(count,0,result,pe.Pe+6,2);
            Put(result,pe.Optional+4,checked(pe.I32(pe.Optional+4)+rawSize));
            Put(result,pe.Optional+56,Align(rva+payload.Length,pe.SectionAlignment));
            Put(result,pe.Optional+64,0); // optional PE checksum; not a driver image
            int entry=pe.Offset(init,6); result[entry]=0xe9; Put(result,entry+1,rva-init-5); result[entry+5]=0x90;
            description="EngineInitialize RVA="+init.ToString("x8")+", input="+functions["input"].ToString("x8")+
                ", memory="+functions["memory"].ToString("x8")+", startup calls="+traces.Length;
            return result;
        }
    }
}
