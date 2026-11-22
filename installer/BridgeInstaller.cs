using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Runtime.InteropServices;
using System.Web.Script.Serialization;

namespace PrototypeModern {
    public sealed class BridgeJournal {
        public int schema=1;
        public string version="0.1.4",state,moduleHash,previousModuleHash,steamRoot,configPath,originalOptions,installedOptions;
    }
    public sealed class BridgeInstaller {
        [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenProcess(uint access,bool inherit,int id);
        [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool QueryFullProcessImageName(IntPtr process,int flags,StringBuilder path,ref int size);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
        internal static string ProcessPath(Process process) {
            IntPtr handle=OpenProcess(0x1000,false,process.Id);
            if(handle==IntPtr.Zero)throw new IOException(InstallerText.ProcessPath);
            try {var path=new StringBuilder(32768);int length=path.Capacity;
                if(!QueryFullProcessImageName(handle,0,path,ref length))throw new IOException(InstallerText.ProcessPath);return path.ToString();
            }finally {CloseHandle(handle);}
        }
        public const string ModuleName="PrototypeModern_Start.exe";
        readonly string root,module,journalPath;
        readonly byte[] payload;
        readonly JavaScriptSerializer json=new JavaScriptSerializer();
        static readonly UTF8Encoding Utf8=new UTF8Encoding(false,true);
        public BridgeInstaller(string directory,byte[] data) {
            root=Path.GetFullPath(directory);module=Path.Combine(root,ModuleName);payload=data;
            journalPath=Path.Combine(root,"PrototypeModern.Backup","bridge-manifest.json");
        }
        static void RejectReparse(string path) {
            if((File.Exists(path)||Directory.Exists(path))&&(File.GetAttributes(path)&FileAttributes.ReparsePoint)!=0)
                throw new IOException(InstallerText.Reparse+path);
        }
        static string HashFile(string path){return PatchImage.Hash(File.ReadAllBytes(path));}
        static string ReadText(string path){return Utf8.GetString(File.ReadAllBytes(path));}
        static void Write(string path,byte[] data) {
            RejectReparse(path);RejectReparse(path+".pmc.tmp");
            using(var file=new FileStream(path+".pmc.tmp",FileMode.Create,FileAccess.Write,FileShare.None)) {
                file.Write(data,0,data.Length);file.Flush(true);
            }
            if(File.Exists(path))File.Replace(path+".pmc.tmp",path,null);else File.Move(path+".pmc.tmp",path);
        }
        void Save(BridgeJournal record){Write(journalPath,Utf8.GetBytes(json.Serialize(record)));}
        BridgeJournal Read() {
            foreach(string path in new[]{root,module,journalPath,Path.GetDirectoryName(journalPath)})RejectReparse(path);
            string readable=File.Exists(journalPath)?journalPath:journalPath+".pmc.tmp";
            if(!File.Exists(readable))return null;
            RejectReparse(readable);var record=json.Deserialize<BridgeJournal>(ReadText(readable));
            if(record==null||record.schema!=1||(record.version!="0.1.3"&&record.version!="0.1.4")||record.moduleHash==null||record.moduleHash.Length!=64||
                (record.previousModuleHash!=null&&(record.previousModuleHash.Length!=64||record.state!="prepared"))||
                String.IsNullOrEmpty(record.steamRoot)||String.IsNullOrEmpty(record.configPath)||String.IsNullOrEmpty(record.installedOptions)||
                (record.state!="prepared"&&record.state!="installed"&&record.state!="restored"))throw new IOException(InstallerText.BridgeRecord);
            string prefix=Path.Combine(Path.GetFullPath(record.steamRoot),"userdata")+Path.DirectorySeparatorChar;
            if(!Path.GetFullPath(record.configPath).StartsWith(prefix,StringComparison.OrdinalIgnoreCase)||
                !String.Equals(Path.GetFileName(record.configPath),"localconfig.vdf",StringComparison.OrdinalIgnoreCase))throw new IOException(InstallerText.SteamConfigPath);
            return record;
        }
        BridgeJournal NewRecord() {
            // The adjacent Steam manifest explicitly records its client path and
            // last owning account. No registry or disk-wide location discovery.
            string steamapps=Directory.GetParent(root).Parent.FullName;
            var manifest=new SteamVdf(ReadText(Path.Combine(steamapps,"appmanifest_10150.acf")));
            if(manifest.Get("AppState","appid")!="10150"||!String.Equals(manifest.Get("AppState","installdir"),Path.GetFileName(root),StringComparison.OrdinalIgnoreCase))
                throw new IOException(InstallerText.AppMismatch);
            string launcher=manifest.Get("AppState","LauncherPath");ulong owner;
            if(String.IsNullOrEmpty(launcher)||!Path.IsPathRooted(launcher)||!File.Exists(launcher)||
                !String.Equals(Path.GetFileName(launcher),"steam.exe",StringComparison.OrdinalIgnoreCase)||
                !UInt64.TryParse(manifest.Get("AppState","LastOwner"),out owner)||owner<76561197960265728UL||owner-76561197960265728UL>UInt32.MaxValue)
                throw new IOException(InstallerText.OwnerMissing);
            string steam=Path.GetDirectoryName(Path.GetFullPath(launcher));
            string config=Path.Combine(steam,"userdata",(owner-76561197960265728UL).ToString(),"config","localconfig.vdf");
            CheckPaths(steam,config);EnsureSteamClosed(steam);
            var document=new SteamVdf(ReadText(config));if(!document.HasPrototype)throw new IOException(InstallerText.GameRecordMissing);
            string original=document.Options;
            if(original!=null&&(original.IndexOf("%command%",StringComparison.OrdinalIgnoreCase)>=0||original.IndexOf(ModuleName,StringComparison.OrdinalIgnoreCase)>=0))
                throw new IOException(InstallerText.WrapperConflict);
            string installed="\""+module+"\" %command%"+(String.IsNullOrEmpty(original)?"":" "+original);
            return new BridgeJournal{moduleHash=PatchImage.Hash(payload),steamRoot=steam,configPath=config,originalOptions=original,installedOptions=installed};
        }
        static void CheckPaths(string steam,string config) {
            RejectReparse(steam);RejectReparse(Path.Combine(steam,"userdata"));
            RejectReparse(Directory.GetParent(config).Parent.FullName);RejectReparse(Path.GetDirectoryName(config));RejectReparse(config);
        }
        static void EnsureSteamClosed(string steam) {
            string expected=Path.Combine(steam,"steam.exe");
            foreach(Process process in Process.GetProcessesByName("steam"))using(process) {
                string path;
                try {path=ProcessPath(process);}catch {if(process.HasExited)continue;throw new IOException(InstallerText.SteamRunningUnknown);}
                if(String.Equals(Path.GetFullPath(path),expected,StringComparison.OrdinalIgnoreCase))
                    throw new IOException(InstallerText.CloseSteam);
            }
        }
        bool AlreadyInstalled(BridgeJournal record) {
            return record!=null&&record.state=="installed"&&record.moduleHash==PatchImage.Hash(payload)&&
                File.Exists(module)&&HashFile(module)==record.moduleHash&&
                new SteamVdf(ReadText(record.configPath)).Options==record.installedOptions;
        }
        void Check(BridgeJournal record,bool installation=false) {
            if(!record.installedOptions.StartsWith("\""+module+"\" %command%",StringComparison.OrdinalIgnoreCase))
                throw new IOException(InstallerText.AppMismatch);
            CheckPaths(record.steamRoot,record.configPath);
            // Replacing our recorded helper does not change Steam's cached
            // command. Only an actual options transaction requires client exit.
            bool matchingOptions=record.state=="installed"&&File.Exists(module)&&
                HashFile(module)==record.moduleHash&&new SteamVdf(ReadText(record.configPath)).Options==record.installedOptions;
            if(!installation||!matchingOptions)EnsureSteamClosed(record.steamRoot);
            if(File.Exists(module)) {
                string hash=HashFile(module);
                if(hash!=record.moduleHash&&(record.state!="prepared"||hash!=record.previousModuleHash))throw new IOException(InstallerText.BridgeForeign);
            }
            string current=new SteamVdf(ReadText(record.configPath)).Options;
            if(current!=record.originalOptions&&current!=record.installedOptions)
                throw new IOException(InstallerText.OptionsEdited);
        }
        void SetOptions(BridgeJournal record,string value) {
            string before=ReadText(record.configPath);
            var document=new SteamVdf(before);
            if(document.Options!=record.originalOptions&&document.Options!=record.installedOptions)throw new IOException(InstallerText.OptionsRaced);
            string after=document.SetOptions(value);if(after==before)return;
            EnsureSteamClosed(record.steamRoot);
            if(ReadText(record.configPath)!=before)throw new IOException(InstallerText.ConfigRaced);
            Write(record.configPath,Utf8.GetBytes(after));
            if(new SteamVdf(ReadText(record.configPath)).Options!=value)throw new IOException(InstallerText.OptionsVerify);
        }
        public void CheckInstall() {
            var record=Read();
            if(record==null) {
                if(File.Exists(module))throw new IOException(InstallerText.BridgeUnowned);
                record=NewRecord();
            }
            Check(record,true);
        }
        public bool IsSteamLocation() {
            var common=Directory.GetParent(root);
            return common!=null&&common.Parent!=null&&
                String.Equals(common.Name,"common",StringComparison.OrdinalIgnoreCase)&&
                String.Equals(common.Parent.Name,"steamapps",StringComparison.OrdinalIgnoreCase)&&
                File.Exists(Path.Combine(common.Parent.FullName,"appmanifest_10150.acf"));
        }
        internal bool InstalledBefore() {var record=Read();return record!=null&&record.state=="installed";}
        public void Install() {
            CheckInstall();var previous=Read();var record=previous==null?NewRecord():json.Deserialize<BridgeJournal>(json.Serialize(previous));
            if(AlreadyInstalled(record))return;
            byte[] previousBytes=File.Exists(module)?File.ReadAllBytes(module):null;
            string previousOptions=new SteamVdf(ReadText(record.configPath)).Options;
            string nextHash=PatchImage.Hash(payload);
            if(record.moduleHash!=nextHash){record.previousModuleHash=record.moduleHash;record.moduleHash=nextHash;}
            Directory.CreateDirectory(Path.GetDirectoryName(journalPath));record.state="prepared";Save(record);
            try {
                if(!File.Exists(module)||HashFile(module)!=record.moduleHash)Write(module,payload);
                if(HashFile(module)!=record.moduleHash)throw new IOException(InstallerText.BridgeVerify);
                SetOptions(record,record.installedOptions);record.version="0.1.4";record.previousModuleHash=null;record.state="installed";Save(record);
            }catch {
                // Restore only our one property, never overwrite the user's
                // entire localconfig (which includes unrelated private data).
                SetOptions(record,previousOptions);
                if(previousBytes!=null)Write(module,previousBytes);
                else if(File.Exists(module)&&HashFile(module)==record.moduleHash)File.Delete(module);
                if(previous!=null)Save(previous);
                else {record.previousModuleHash=null;record.state="restored";Save(record);}
                throw;
            }
        }
        public void CheckRestore(){var record=Read();if(record!=null)Check(record);}
        public void Restore() {
            var record=Read();if(record==null)return;Check(record);
            SetOptions(record,record.originalOptions);
            if(File.Exists(module))File.Delete(module);
            record.previousModuleHash=null;record.state="restored";Save(record);
        }
        public string Inspect(string input,string output) {
            byte[] original=File.ReadAllBytes(input);LauncherPatch.ValidateOriginal(original);
            if(output!=null) {
                if(String.Equals(Path.GetFullPath(input),Path.GetFullPath(output),StringComparison.OrdinalIgnoreCase))throw new IOException(InstallerText.InspectInput);
                Write(output,original);
            }
            return json.Serialize(new{version="0.1.4",mode="static-only",gameLaunched=false,originalSha256=PatchImage.Hash(original),
                outputSha256=PatchImage.Hash(original),moduleSha256=PatchImage.Hash(payload),launcherModified=false,
                details="Steam per-game command bridge; guarded cancellation of verified startup receive; original EXE unchanged"});
        }
    }
}
