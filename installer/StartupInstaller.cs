using System;
using System.IO;
using System.Text;
using System.Web.Script.Serialization;

namespace PrototypeModern {
    public sealed class StartupJournal {
        public int schema=1;
        public string version="0.1.1",state,originalHash,patchedHash,moduleHash,details;
    }
    public sealed class StartupInstaller {
        readonly string root,launcher,module,backup,journalPath,originalPath;
        readonly byte[] payload;
        readonly JavaScriptSerializer json=new JavaScriptSerializer();
        public StartupInstaller(string directory,byte[] bytes) {
            root=Path.GetFullPath(directory);payload=bytes;
            launcher=Path.Combine(root,"prototypef.exe");module=Path.Combine(root,"PMNet9.dll");
            backup=Path.Combine(root,"PrototypeModern.Backup");
            journalPath=Path.Combine(backup,"startup-manifest.json");
            originalPath=Path.Combine(backup,"prototypef.exe.original");
        }
        static string HashFile(string path){return PatchImage.Hash(File.ReadAllBytes(path));}
        static void RejectReparse(string path) {
            if((File.Exists(path)||Directory.Exists(path))&&(File.GetAttributes(path)&FileAttributes.ReparsePoint)!=0)
                throw new IOException(InstallerText.Reparse+path);
        }
        static void Write(string path,byte[] bytes) {
            RejectReparse(path);RejectReparse(path+".tmp");
            using(var stream=new FileStream(path+".tmp",FileMode.Create,FileAccess.Write,FileShare.None)) {
                stream.Write(bytes,0,bytes.Length);stream.Flush(true);
            }
            if(File.Exists(path))File.Replace(path+".tmp",path,null);else File.Move(path+".tmp",path);
        }
        void Save(StartupJournal record){Write(journalPath,Encoding.UTF8.GetBytes(json.Serialize(record)));}
        static bool ValidHash(string value) {
            if(value==null||value.Length!=64)return false;
            foreach(char c in value)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
            return true;
        }
        StartupJournal Read() {
            foreach(string path in new[]{root,launcher,module,backup,journalPath,originalPath})RejectReparse(path);
            string readable=File.Exists(journalPath)?journalPath:journalPath+".tmp";
            if(!File.Exists(readable))return null;
            RejectReparse(readable);
            var record=json.Deserialize<StartupJournal>(File.ReadAllText(readable,Encoding.UTF8));
            if(record==null||record.schema!=1||record.version!="0.1.1"||
                !ValidHash(record.originalHash)||!ValidHash(record.patchedHash)||!ValidHash(record.moduleHash)||
                (record.state!="prepared"&&record.state!="installed"&&record.state!="restored"))
                throw new IOException(InstallerText.StartupRecord);
            return record;
        }
        void VerifyModule(StartupJournal record) {
            if(File.Exists(module)&&HashFile(module)!=record.moduleHash)
                throw new IOException(InstallerText.LegacyForeign);
        }
        void VerifyBackup(StartupJournal record) {
            if(!File.Exists(originalPath)||HashFile(originalPath)!=record.originalHash)
                throw new IOException(InstallerText.LegacyBackup);
        }
        public void CheckInstall() {
            var record=Read();string current=HashFile(launcher);
            if(record==null) {
                if(File.Exists(module)||File.Exists(originalPath))throw new IOException(InstallerText.LegacyName);
                string details;LauncherPatch.Create(File.ReadAllBytes(launcher),out details);
            } else {
                VerifyModule(record);
                if(record.moduleHash!=PatchImage.Hash(payload))throw new IOException(InstallerText.LegacyBuild);
                if(current!=record.originalHash&&current!=record.patchedHash)
                    throw new IOException(InstallerText.LegacyChanged);
                if(current==record.patchedHash||File.Exists(originalPath))VerifyBackup(record);
            }
        }
        public void Install() {
            CheckInstall();var record=Read();byte[] current=File.ReadAllBytes(launcher);
            if(record!=null&&PatchImage.Hash(current)==record.patchedHash) {
                if(!File.Exists(module))Write(module,payload);
                record.state="installed";Save(record);return;
            }
            string details;byte[] patched=LauncherPatch.Create(current,out details);
            if(record==null) {
                record=new StartupJournal{state="prepared",originalHash=PatchImage.Hash(current),
                    patchedHash=PatchImage.Hash(patched),moduleHash=PatchImage.Hash(payload),details=details};
                Directory.CreateDirectory(backup);
            } else if(record.patchedHash!=PatchImage.Hash(patched))throw new IOException(InstallerText.LegacyRebuild);
            record.state="prepared";Save(record);
            try {
                if(!File.Exists(originalPath))Write(originalPath,current);
                VerifyBackup(record);
                if(!File.Exists(module))Write(module,payload);
                VerifyModule(record);
                if(HashFile(launcher)!=record.originalHash)throw new IOException(InstallerText.LegacyDuring);
                Write(launcher,patched);
                if(HashFile(launcher)!=record.patchedHash)throw new IOException(InstallerText.LegacyVerify);
                record.state="installed";Save(record);
            } catch(Exception error) {
                try {Restore();} catch(Exception recovery) {
                    throw new IOException(error.Message+InstallerText.LegacyJournal+recovery.Message,error);
                }
                throw;
            }
        }
        public void CheckRestore() {
            var record=Read();if(record==null||record.state=="restored")return;
            VerifyModule(record);string current=HashFile(launcher);
            if(current!=record.originalHash&&current!=record.patchedHash)
                throw new IOException(InstallerText.LegacyRestoreConflict);
            // If interruption happened before the original backup was written,
            // an untouched original launcher is sufficient to abandon the transaction.
            if(current!=record.originalHash||File.Exists(originalPath))VerifyBackup(record);
        }
        public void Restore() {
            CheckRestore();var record=Read();if(record==null||record.state=="restored")return;
            if(HashFile(launcher)==record.patchedHash)Write(launcher,File.ReadAllBytes(originalPath));
            if(HashFile(launcher)!=record.originalHash)throw new IOException(InstallerText.LegacyRestoreVerify);
            if(File.Exists(module))File.Delete(module);
            record.state="restored";Save(record);
        }
        public string Inspect(string input,string output) {
            string details;byte[] original=File.ReadAllBytes(input),patched=LauncherPatch.Create(original,out details);
            if(output!=null) {
                if(String.Equals(Path.GetFullPath(input),Path.GetFullPath(output),StringComparison.OrdinalIgnoreCase))
                    throw new IOException(InstallerText.InspectInput);
                Write(output,patched);
            }
            return json.Serialize(new{version="0.1.1",mode="static-only",gameLaunched=false,
                originalSha256=PatchImage.Hash(original),patchedSha256=PatchImage.Hash(patched),
                moduleSha256=PatchImage.Hash(payload),originalSize=original.Length,patchedSize=patched.Length,details=details});
        }
    }
}
