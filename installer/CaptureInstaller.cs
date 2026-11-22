using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;
using System.Web.Script.Serialization;

namespace PrototypeModern {
    internal sealed class CaptureRecord {
        public int schema=1;
        public Dictionary<string,string> files;
        public string iniHash;
    }

    // A pre-existing INI belongs to the player or their development setup.
    // A clean install gets the same bounded collector with a private Python runtime.
    internal sealed class CaptureInstaller {
        readonly string folder, ini, journal;
        readonly JavaScriptSerializer json=new JavaScriptSerializer();
        Dictionary<string,byte[]> payload;

        public CaptureInstaller(string root) {
            folder=Path.Combine(root,"PrototypeModern.Capture");
            ini=Path.Combine(root,"PrototypeModern.Performance.ini");
            journal=Path.Combine(root,"PrototypeModern.Backup","capture-manifest.json");
        }
        Dictionary<string,byte[]> Payload() {
            if(payload!=null)return payload;
            var files=new Dictionary<string,byte[]>(StringComparer.OrdinalIgnoreCase);
            using(var zip=new ZipArchive(new MemoryStream(Installer.Resource("captureRuntime")))) {
                foreach(var entry in zip.Entries) {
                    if(entry.Name!=entry.FullName || String.IsNullOrEmpty(entry.Name))throw new InvalidDataException(InstallerText.CaptureArchive);
                    using(var source=entry.Open())using(var bytes=new MemoryStream()) {
                        source.CopyTo(bytes);files.Add(entry.Name,bytes.ToArray());
                    }
                }
            }
            files.Add("auto_capture.ps1",Installer.Resource("captureBootstrap"));
            files.Add("capture_text.py",Installer.Resource("captureText"));
            files.Add("capture_engine.py",Installer.Resource("captureEngine"));
            payload=files;
            return files;
        }
        byte[] Config() {
            string value="[Capture]\r\nEnabled=1\r\nBootstrap="+Path.Combine(folder,"auto_capture.ps1")+"\r\n";
            return Encoding.Unicode.GetPreamble().Concat(Encoding.Unicode.GetBytes(value)).ToArray();
        }
        CaptureRecord Read() {
            foreach(string path in new[]{ini,folder,journal,Path.GetDirectoryName(journal)})Installer.RejectReparse(path);
            if(!File.Exists(journal))return null;
            var item=json.Deserialize<CaptureRecord>(File.ReadAllText(journal,Encoding.UTF8));
            if(item==null || item.schema!=1 || item.files==null || !Digest(item.iniHash) ||
                !item.files.ContainsKey("auto_capture.ps1"))throw new InvalidDataException(InstallerText.CaptureRecord);
            foreach(var file in item.files) {
                if(String.IsNullOrWhiteSpace(file.Key)||file.Key=="."||file.Key==".."||
                    file.Key.IndexOfAny(Path.GetInvalidFileNameChars())>=0||Path.GetFileName(file.Key)!=file.Key||
                    !Digest(file.Value))throw new InvalidDataException(InstallerText.CaptureRecord);
            }
            return item;
        }
        static bool Digest(string value) {
            return value!=null&&value.Length==64&&value.All(c=>(c>='0'&&c<='9')||(c>='a'&&c<='f'));
        }
        static void VerifyFile(string path,string hash) {
            Installer.RejectReparse(path);
            if(Directory.Exists(path)||(File.Exists(path)&&PatchImage.Hash(File.ReadAllBytes(path))!=hash))
                throw new IOException(InstallerText.CaptureModified+Path.GetFileName(path));
        }
        void Verify(CaptureRecord item) {
            VerifyFile(ini,item.iniHash);
            foreach(var file in item.files)VerifyFile(Path.Combine(folder,file.Key),file.Value);
        }
        public void CheckInstall() {
            var item=Read();
            if(item!=null){Verify(item);return;}
            if(!Environment.Is64BitOperatingSystem||File.Exists(ini))return;
            if(File.Exists(folder)||Directory.Exists(folder)||Directory.Exists(ini))throw new IOException(InstallerText.CaptureConflict);
        }
        internal bool WouldAdd() {return Environment.Is64BitOperatingSystem&&!File.Exists(journal)&&!File.Exists(ini);}
        public bool Install() {
            var item=Read();
            if(item==null&&(!Environment.Is64BitOperatingSystem||File.Exists(ini)))return false;
            CheckInstall();
            var files=Payload();byte[] config=Config();
            var hashes=files.ToDictionary(f=>f.Key,f=>PatchImage.Hash(f.Value));
            bool fresh=item==null;
            if(fresh) {
                item=new CaptureRecord{files=hashes,iniHash=PatchImage.Hash(config)};
                Directory.CreateDirectory(Path.GetDirectoryName(journal));
                Installer.AtomicWrite(journal,Encoding.UTF8.GetBytes(json.Serialize(item)));
            } else if(item.iniHash!=PatchImage.Hash(config)||item.files.Count!=hashes.Count||
                    hashes.Any(f=>!item.files.ContainsKey(f.Key)||item.files[f.Key]!=f.Value))
                throw new IOException(InstallerText.CaptureDifferent);
            try {
                Directory.CreateDirectory(folder);
                foreach(var file in files) {
                    string path=Path.Combine(folder,file.Key);
                    if(!File.Exists(path))Installer.AtomicWrite(path,file.Value);
                }
                if(!File.Exists(ini))Installer.AtomicWrite(ini,config);
                Verify(item);
                return fresh;
            } catch {
                if(fresh)Restore();
                throw;
            }
        }
        public void CheckRestore() {var item=Read();if(item!=null)Verify(item);}
        public void Restore() {
            var item=Read();if(item==null)return;
            Verify(item);
            if(File.Exists(ini))File.Delete(ini);
            foreach(var name in item.files.Keys) {
                string path=Path.Combine(folder,name);
                if(File.Exists(path))File.Delete(path);
            }
            if(Directory.Exists(folder)&&Directory.GetFileSystemEntries(folder).Length==0)Directory.Delete(folder);
            File.Delete(journal);
        }
    }
}
