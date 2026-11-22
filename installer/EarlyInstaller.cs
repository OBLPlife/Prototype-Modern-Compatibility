using System;
using System.IO;
using System.Text;
using System.Web.Script.Serialization;

namespace PrototypeModern {
    public sealed class EarlyJournal {
        public int schema=1;
        public string version="0.1.2",state,moduleHash,launcherHashAtInstall;
    }
    public sealed class EarlyInstaller {
        readonly string root,module,journalPath;
        readonly byte[] payload;
        readonly JavaScriptSerializer json=new JavaScriptSerializer();
        public EarlyInstaller(string directory,byte[] data) {
            root=Path.GetFullPath(directory);payload=data;module=Path.Combine(root,"version.dll");
            journalPath=Path.Combine(root,"PrototypeModern.Backup","early-manifest.json");
        }
        static void RejectReparse(string path) {
            if((File.Exists(path)||Directory.Exists(path))&&(File.GetAttributes(path)&FileAttributes.ReparsePoint)!=0)
                throw new IOException(InstallerText.Reparse+path);
        }
        static string HashFile(string path){return PatchImage.Hash(File.ReadAllBytes(path));}
        static void Write(string path,byte[] data) {
            RejectReparse(path);RejectReparse(path+".tmp");
            using(var file=new FileStream(path+".tmp",FileMode.Create,FileAccess.Write,FileShare.None)) {
                file.Write(data,0,data.Length);file.Flush(true);
            }
            if(File.Exists(path))File.Replace(path+".tmp",path,null);else File.Move(path+".tmp",path);
        }
        void Save(EarlyJournal record){Write(journalPath,Encoding.UTF8.GetBytes(json.Serialize(record)));}
        EarlyJournal Read() {
            foreach(string path in new[]{root,module,journalPath,Path.GetDirectoryName(journalPath)})RejectReparse(path);
            string readable=File.Exists(journalPath)?journalPath:journalPath+".tmp";
            if(!File.Exists(readable))return null;
            RejectReparse(readable);
            var record=json.Deserialize<EarlyJournal>(File.ReadAllText(readable,Encoding.UTF8));
            if(record==null||record.schema!=1||record.version!="0.1.2"||record.moduleHash==null||record.moduleHash.Length!=64||
                (record.state!="prepared"&&record.state!="installed"&&record.state!="restored"))
                throw new IOException(InstallerText.EarlyRecord);
            return record;
        }
        void VerifyOwned(EarlyJournal record) {
            if(File.Exists(module)&&(record==null||HashFile(module)!=record.moduleHash))
                throw new IOException(InstallerText.OtherProxy);
        }
        public void CheckInstall() {
            var record=Read();VerifyOwned(record);
            if(record!=null&&record.moduleHash!=PatchImage.Hash(payload))
                throw new IOException(InstallerText.OtherEarlyBuild);
            LauncherPatch.ValidateOriginal(File.ReadAllBytes(Path.Combine(root,"prototypef.exe")));
        }
        public void Install() {
            CheckInstall();var record=Read()??new EarlyJournal{moduleHash=PatchImage.Hash(payload)};
            record.launcherHashAtInstall=HashFile(Path.Combine(root,"prototypef.exe"));
            record.state="prepared";Directory.CreateDirectory(Path.GetDirectoryName(journalPath));Save(record);
            try {
                if(!File.Exists(module))Write(module,payload);
                VerifyOwned(record);
                if(HashFile(Path.Combine(root,"prototypef.exe"))!=record.launcherHashAtInstall)
                    throw new IOException(InstallerText.LauncherRaced);
                record.state="installed";Save(record);
            } catch {
                if(File.Exists(module)&&HashFile(module)==record.moduleHash)File.Delete(module);
                record.state="restored";Save(record);throw;
            }
        }
        public void CheckRestore(){var record=Read();if(record!=null&&record.state!="restored")VerifyOwned(record);}
        public void Restore() {
            var record=Read();if(record==null||record.state=="restored")return;VerifyOwned(record);
            if(File.Exists(module))File.Delete(module);
            record.state="restored";Save(record);
        }
        public string Inspect(string input,string output) {
            byte[] original=File.ReadAllBytes(input);LauncherPatch.ValidateOriginal(original);
            if(output!=null) {
                if(String.Equals(Path.GetFullPath(input),Path.GetFullPath(output),StringComparison.OrdinalIgnoreCase))
                    throw new IOException(InstallerText.InspectInput);
                Write(output,original);
            }
            return json.Serialize(new{version="0.1.2",mode="static-only",gameLaunched=false,
                originalSha256=PatchImage.Hash(original),outputSha256=PatchImage.Hash(original),
                moduleSha256=PatchImage.Hash(payload),launcherModified=false,
                details="game-local version.dll; launcher bytes untouched; receive hook is memory-only"});
        }
    }
}
