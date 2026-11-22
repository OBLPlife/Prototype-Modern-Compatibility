using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Web.Script.Serialization;

namespace PrototypeModern {
    public sealed class InstallReview {
        public bool NeedsWarning {get; internal set;}
        public string WarningText {get{return NeedsWarning?InstallerText.ContinueRisk:String.Empty;}}
        internal string Root;
        internal long EngineLength,EngineWriteTime;
        internal bool Variant,Steam,Capture,Helper,HasEntry;
        internal byte[] EngineInput,EngineOutput;
    }

    internal sealed class ResourceEntry {
        public string name,original,installed,previous;
    }
    internal sealed class ResourceRecord {
        public int schema=1;
        public string state;
        public bool ownsSteam,ownsCapture;
        public List<ResourceEntry> files=new List<ResourceEntry>();
    }

    // Local transaction for installations that use a different distribution or resource layout.
    // Hashes here name copies from this directory. They are never compared to a retail release.
    internal sealed class ResourceInstaller {
        internal const string Shortcut="PrototypeModern_Play.lnk";
        static readonly string[] Names={Installer.EngineName,Installer.ModuleName,BridgeInstaller.ModuleName,Shortcut};
        readonly string root,backups,journal;
        readonly BridgeInstaller bridge;
        readonly CaptureInstaller capture;
        readonly byte[] module,helper;
        readonly JavaScriptSerializer json=new JavaScriptSerializer();

        internal ResourceInstaller(string rootDir,BridgeInstaller b,CaptureInstaller c,byte[] data,byte[] startup) {
            root=rootDir;bridge=b;capture=c;module=data;helper=startup;
            backups=Path.Combine(root,"PrototypeModern.ResourceBackup");
            journal=Path.Combine(backups,"manifest.json");
        }
        internal bool HasRecord {get{return File.Exists(journal);}}
        internal byte[] EngineBaseline(BuildProfile profile) {
            var rec=Read();
            if(rec==null||rec.state=="restored")return null;
            var entry=rec.files.Find(f=>f.name==Installer.EngineName);
            if(entry==null||entry.original==null)return null;
            byte[] bytes=File.ReadAllBytes(Blob(entry.original));
            if(PatchImage.Hash(bytes)!=entry.original)throw new IOException(InstallerText.BackupInvalid+entry.name);
            if(Current(entry.name)==entry.installed)return bytes;
            if(!File.Exists(Full(entry.name)))return null;
            try{return PatchImage.BeforeOwnBootstrap(bytes,File.ReadAllBytes(Full(entry.name)),profile);}
            catch(InvalidDataException){return null;}
        }
        internal bool Owns(string name) {
            var rec=Read();
            if(rec==null||rec.state=="restored")return false;
            return rec.files.Any(f=>f.name==name && Current(name)==f.installed);
        }
        internal bool HasChanges() {
            var rec=Read();
            return rec!=null && rec.state!="restored" &&
                rec.files.Any(f=>f.name!=Installer.EngineName&&Current(f.name)!=f.installed);
        }
        internal bool HasShortcut {get{return Owns(Shortcut);}}
        ResourceRecord Read() {
            Installer.RejectReparse(backups);Installer.RejectReparse(journal);
            if(!File.Exists(journal))return null;
            var r=json.Deserialize<ResourceRecord>(File.ReadAllText(journal,Encoding.UTF8));
            if(r==null||r.schema!=1||r.files==null||(r.state!="prepared"&&r.state!="installed"&&r.state!="restored")||
                r.files.Select(f=>f.name).Distinct().Count()!=r.files.Count||
                r.files.Any(f=>!Names.Contains(f.name)||!IsHash(f.installed)||(f.original!=null&&!IsHash(f.original))||(f.previous!=null&&!IsHash(f.previous))))
                throw new InvalidDataException(InstallerText.JournalInvalid);
            return r;
        }
        static bool IsHash(string s){return s!=null&&s.Length==64&&s.All(c=>(c>='0'&&c<='9')||(c>='a'&&c<='f'));}
        void Save(ResourceRecord r){Installer.AtomicWrite(journal,Encoding.UTF8.GetBytes(json.Serialize(r)));}
        string Full(string name){var path=Path.Combine(root,name);Installer.RejectReparse(path);return path;}
        string Current(string name){string p=Full(name);return File.Exists(p)?PatchImage.Hash(File.ReadAllBytes(p)):null;}
        string Blob(string hash){string path=Path.Combine(backups,hash+".original");Installer.RejectReparse(path);return path;}
        string Snapshot(string name) {
            string path=Full(name);if(!File.Exists(path))return null;
            var bytes=File.ReadAllBytes(path);string hash=PatchImage.Hash(bytes),copy=Blob(hash);
            if(!File.Exists(copy))Installer.AtomicWrite(copy,bytes);
            if(PatchImage.Hash(File.ReadAllBytes(copy))!=hash)throw new IOException(InstallerText.BackupInvalid+name);
            return hash;
        }
        void Undo(string name,string hash) {
            string path=Full(name);
            if(hash==null){if(File.Exists(path))File.Delete(path);return;}
            byte[] bytes=File.ReadAllBytes(Blob(hash));
            if(PatchImage.Hash(bytes)!=hash)throw new IOException(InstallerText.BackupInvalid+name);
            if(Current(name)!=hash)Installer.AtomicWrite(path,bytes);
        }
        static void Set(object target,string member,object value) {
            target.GetType().InvokeMember(member,BindingFlags.SetProperty,null,target,new[]{value});
        }
        byte[] MakeShortcut() {
            string scratch=Path.Combine(Path.GetTempPath(),Guid.NewGuid().ToString("N")+".lnk");
            object shell=null,link=null;
            try {
                shell=Activator.CreateInstance(Type.GetTypeFromProgID("WScript.Shell",true));
                link=shell.GetType().InvokeMember("CreateShortcut",BindingFlags.InvokeMethod,null,shell,new object[]{scratch});
                Set(link,"TargetPath",Full(BridgeInstaller.ModuleName));
                Set(link,"Arguments","\""+Full("prototypef.exe")+"\"");
                Set(link,"WorkingDirectory",root);
                Set(link,"IconLocation",Full("prototypef.exe")+",0");
                link.GetType().InvokeMember("Save",BindingFlags.InvokeMethod,null,link,null);
                return File.ReadAllBytes(scratch);
            } finally {
                if(link!=null)Marshal.FinalReleaseComObject(link);
                if(shell!=null)Marshal.FinalReleaseComObject(shell);
                if(File.Exists(scratch))File.Delete(scratch);
            }
        }
        internal string Install(InstallReview plan) {
            var old=Read();
            var record=old==null||old.state=="restored"?new ResourceRecord():json.Deserialize<ResourceRecord>(json.Serialize(old));
            var files=new Dictionary<string,byte[]>();
            if(plan.EngineOutput!=null)files.Add(Installer.EngineName,plan.EngineOutput);
            files.Add(Installer.ModuleName,module);
            if(!plan.Steam&&plan.Helper){
                files.Add(BridgeInstaller.ModuleName,helper);
                files.Add(Shortcut,Owns(Shortcut)?File.ReadAllBytes(Full(Shortcut)):MakeShortcut());
            }
            Directory.CreateDirectory(backups);
            foreach(var f in files) {
                string before=Snapshot(f.Key);var entry=record.files.Find(x=>x.name==f.Key);
                if(f.Key==Installer.EngineName&&before!=PatchImage.Hash(plan.EngineInput))throw new IOException(InstallerText.ReviewAgain);
                if(entry==null){entry=new ResourceEntry{name=f.Key,original=before};record.files.Add(entry);}
                else if(before!=entry.installed && (old==null||old.state!="prepared"||before!=entry.previous))entry.original=before;
                entry.previous=before;entry.installed=PatchImage.Hash(f.Value);
            }
            // A modified, already bootstrapped engine can be reused in place. No claim on outside edits.
            if(!files.ContainsKey(Installer.EngineName))
                record.files.RemoveAll(x=>x.name==Installer.EngineName && Current(x.name)!=x.installed);
            record.state="prepared";
            bool newSteam=plan.Steam&&!bridge.InstalledBefore();
            bool newCapture=plan.Capture&&capture.WouldAdd();
            record.ownsSteam|=newSteam;record.ownsCapture|=newCapture;
            Save(record);
            try {
                foreach(var f in files) {
                    var entry=record.files.Find(x=>x.name==f.Key);
                    if(Current(f.Key)!=entry.previous)throw new IOException(InstallerText.ReviewAgain);
                    if(entry.previous!=entry.installed)Installer.AtomicWrite(Full(f.Key),f.Value);
                    if(Current(f.Key)!=entry.installed)throw new IOException(InstallerText.InstallVerifyFailed);
                }
                if(plan.Capture)capture.Install();
                if(plan.Steam)bridge.Install();
                record.state="installed";foreach(var e in record.files)e.previous=null;Save(record);
            } catch(Exception failure) {
                var errors=new List<string>();
                if(newSteam)try{bridge.Restore();}catch(Exception e){errors.Add(e.Message);}
                if(newCapture)try{capture.Restore();}catch(Exception e){errors.Add(e.Message);}
                foreach(var f in files.Keys.Reverse()){
                    try {
                        var entry=record.files.Find(x=>x.name==f);
                        string now=Current(f);
                        if(now==entry.previous)continue;
                        if(now!=entry.installed)throw new IOException(InstallerText.ResourceConflict+f);
                        Undo(f,entry.previous);
                    }catch(Exception e){errors.Add(e.Message);}
                }
                if(errors.Count!=0)throw new IOException(failure.Message+Environment.NewLine+InstallerText.RollbackIncomplete+String.Join(Environment.NewLine,errors),failure);
                if(old!=null)Save(old);
                else {record.state="restored";record.ownsSteam=record.ownsCapture=false;Save(record);}
                throw;
            }
            string result=plan.Steam?InstallerText.Installed:plan.Helper?InstallerText.LocalDone:InstallerText.NativeDone;
            return plan.HasEntry?result:result+Environment.NewLine+InstallerText.CorePartial;
        }
        internal string Restore() {
            var r=Read();if(r==null||r.state=="restored")return InstallerText.VariantRestored;
            foreach(var e in r.files) {
                string now=Current(e.name);
                if(now!=e.installed&&now!=e.original&&(r.state!="prepared"||now!=e.previous))throw new IOException(InstallerText.ResourceConflict+e.name);
                if(e.original!=null&&(!File.Exists(Blob(e.original))||PatchImage.Hash(File.ReadAllBytes(Blob(e.original)))!=e.original))
                    throw new IOException(InstallerText.BackupInvalid+e.name);
            }
            if(r.ownsSteam)bridge.CheckRestore();
            if(r.ownsCapture)capture.CheckRestore();
            if(r.ownsSteam)bridge.Restore();
            if(r.ownsCapture)capture.Restore();
            foreach(var entry in r.files.AsEnumerable().Reverse())Undo(entry.name,entry.original);
            r.state="restored";Save(r);
            return InstallerText.VariantRestored;
        }
    }
}

