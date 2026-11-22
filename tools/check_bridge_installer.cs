using System;
using System.IO;
using System.Text;
using System.Web.Script.Serialization;
using PrototypeModern;
class CheckBridgeInstaller {
    static void Require(bool ok,string name){if(!ok)throw new Exception(name);Console.WriteLine("PASS "+name);}
    static string Hash(string path){return PatchImage.Hash(File.ReadAllBytes(path));}
    static string Fixture(string options) {
        return "\"UserLocalConfigStore\"\r\n{\r\n\t\"Unrelated\" \"preserve-me\"\r\n\t\"Software\"\r\n\t{\r\n\t\t\"Valve\"\r\n\t\t{\r\n\t\t\t\"Steam\"\r\n\t\t\t{\r\n\t\t\t\t\"apps\"\r\n\t\t\t\t{\r\n\t\t\t\t\t\"10150\"\r\n\t\t\t\t\t{\r\n\t\t\t\t\t\t\"Playtime\" \"1\"\r\n"+
            (options==null?"":"\t\t\t\t\t\t\"LaunchOptions\" \""+options+"\"\r\n")+
            "\t\t\t\t\t}\r\n\t\t\t\t}\r\n\t\t\t}\r\n\t\t}\r\n\t}\r\n}\r\n";
    }
    static int Main(string[] args) {
        try {
            if(args.Length==2&&args[0]=="--read-only") {
                var actual=new SteamVdf(new UTF8Encoding(false,true).GetString(File.ReadAllBytes(args[1])));
                Require(actual.HasPrototype,"actual Steam file parses; target game object exists (no content emitted)");
                string edited=actual.SetOptions("\"C:\\test space\\bridge.exe\" %command%");
                Require(new SteamVdf(edited).SetOptions(actual.Options)==new UTF8Encoding(false,true).GetString(File.ReadAllBytes(args[1])),"actual file in-memory edit/restore preserves all original bytes");return 0;
            }
            if(args.Length!=3)throw new Exception("Need fixture Steam root, original engine and original launcher.");
            string steam=Path.GetFullPath(args[0]),root=Path.Combine(steam,"steamapps","common","Prototype");
            if(Directory.Exists(steam))throw new Exception("Fixture root must be new.");
            string config=Path.Combine(steam,"userdata","123456789","config","localconfig.vdf");
            Directory.CreateDirectory(root);Directory.CreateDirectory(Path.GetDirectoryName(config));
            File.WriteAllText(Path.Combine(steam,"steam.exe"),"OFFLINE FIXTURE - NOT EXECUTABLE");
            File.WriteAllText(Path.Combine(steam,"steamapps","appmanifest_10150.acf"),"\"AppState\"{\"appid\"\"10150\"\"installdir\"\"Prototype\"\"LauncherPath\"\""+Path.Combine(steam,"steam.exe").Replace("\\","\\\\")+"\"\"LastOwner\"\"76561198083722517\"}");
            File.WriteAllText(config,Fixture(null),new UTF8Encoding(false));
            File.Copy(args[1],Path.Combine(root,"prototypeenginef.dll"));File.Copy(args[2],Path.Combine(root,"prototypef.exe"));
            foreach(string name in new[]{"binkw32.dll","art.rcf","skuinfo.p3d"})File.WriteAllBytes(Path.Combine(root,name),new byte[0]);
            File.WriteAllText(Path.Combine(root,".prototype-modern-offline-fixture"),"No original game code is executed.");
            string originalEngine=Hash(Path.Combine(root,"prototypeenginef.dll")),originalLauncher=Hash(Path.Combine(root,"prototypef.exe")),originalConfig=Hash(config);
            foreach(string option in new[]{null,"","-windowed","-path \\\"C:\\\\space dir\\\\tail\\\\\\\""}) {
                string input=Fixture(option);var document=new SteamVdf(input);
                string edited=document.SetOptions("\"D:\\space 中文\\PrototypeModern_Start.exe\" %command%");
                Require(new SteamVdf(edited).SetOptions(document.Options)==input,"VDF edit/restore exact roundtrip, original option="+(option==null?"absent":"present"));
            }
            var installer=new Installer(root);installer.Install();
            string module=Path.Combine(root,BridgeInstaller.ModuleName);byte[] payload=File.ReadAllBytes(module);
            string patchedEngine=Hash(Path.Combine(root,"prototypeenginef.dll")),installedConfig=Hash(config);
            Require(patchedEngine!=originalEngine&&Hash(Path.Combine(root,"prototypef.exe"))==originalLauncher,"engine patched while signed launcher is byte-identical");
            Require(new SteamVdf(File.ReadAllText(config)).Options.Contains("%command%"),"only game launch option gains bridge");
            installer.Install();Require(Hash(config)==installedConfig&&Hash(Path.Combine(root,"prototypeenginef.dll"))==patchedEngine,"repeat installation is unchanged");
            string stableJournal=Path.Combine(root,"PrototypeModern.Backup","bridge-manifest.json");
            string journalHash=Hash(stableJournal);
            using(var configReadOnly=new FileStream(config,FileMode.Open,FileAccess.Read,FileShare.Read))
            using(var helperReadOnly=new FileStream(module,FileMode.Open,FileAccess.Read,FileShare.Read))
            using(var journalReadOnly=new FileStream(stableJournal,FileMode.Open,FileAccess.Read,FileShare.Read)) {
                installer.Install();
                Require(Hash(config)==installedConfig&&Hash(stableJournal)==journalHash,
                    "unchanged installed bridge is a read-only no-op; locked helper/config/journal remain byte-identical");
            }
            string engineModule=Path.Combine(root,Installer.ModuleName),engineJournal=Path.Combine(root,"PrototypeModern.Backup","manifest.json");
            var serializer=new JavaScriptSerializer();
            byte[] oldHelper=(byte[])payload.Clone();oldHelper[2]^=1; // fixture data, never run
            var helperRecord=serializer.Deserialize<BridgeJournal>(File.ReadAllText(stableJournal));
            File.WriteAllBytes(module,oldHelper);helperRecord.version="0.1.3";helperRecord.moduleHash=PatchImage.Hash(oldHelper);
            File.WriteAllText(stableJournal,serializer.Serialize(helperRecord));
            using(var lockedConfig=new FileStream(config,FileMode.Open,FileAccess.Read,FileShare.Read)) {
                installer.Install();
                Require(Hash(module)==PatchImage.Hash(payload)&&Hash(config)==installedConfig&&
                    serializer.Deserialize<BridgeJournal>(File.ReadAllText(stableJournal)).version=="0.1.4",
                    "owned helper upgrade succeeds with config write-locked; Steam launch options remain byte-identical");
            }
            string oldModulePath=Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"PrototypeModern-0.1.0.dll");
            byte[] oldModule=File.Exists(oldModulePath)?File.ReadAllBytes(oldModulePath):File.ReadAllBytes(engineModule);
            if(!File.Exists(oldModulePath))oldModule[2]^=1; // synthetic ownership fixture; never executed
            string newModuleHash=Hash(engineModule);var owned=serializer.Deserialize<Journal>(File.ReadAllText(engineJournal));
            File.WriteAllBytes(engineModule,oldModule);owned.moduleHash=PatchImage.Hash(oldModule);File.WriteAllText(engineJournal,serializer.Serialize(owned));
            installer.Install();Require(Hash(engineModule)==newModuleHash&&Hash(Path.Combine(root,"prototypeenginef.dll"))==patchedEngine,"owned previous engine module upgrades without rewriting bootstrap or original backup");
            owned=serializer.Deserialize<Journal>(File.ReadAllText(engineJournal));owned.state="prepared";owned.previousModuleHash=PatchImage.Hash(oldModule);
            File.WriteAllText(engineJournal,serializer.Serialize(owned));File.WriteAllBytes(engineModule,oldModule);
            installer.Install();Require(Hash(engineModule)==newModuleHash&&serializer.Deserialize<Journal>(File.ReadAllText(engineJournal)).previousModuleHash==null,"interrupted module upgrade resumes from previous owned bytes");
            string bridgeJournal=Path.Combine(root,"PrototypeModern.Backup","bridge-manifest.json");
            var previousBridge=serializer.Deserialize<BridgeJournal>(File.ReadAllText(bridgeJournal));
            byte[] oldBridge=(byte[])payload.Clone();oldBridge[2]^=1; // private non-executed ownership fixture
            previousBridge.moduleHash=PatchImage.Hash(oldBridge);File.WriteAllText(bridgeJournal,serializer.Serialize(previousBridge));File.WriteAllBytes(module,oldBridge);
            installer.Install();Require(Hash(module)==PatchImage.Hash(payload)&&Hash(config)==installedConfig,"owned bridge upgrade preserves Steam options and signed launcher");
            File.Delete(module);installer.Install();Require(Hash(module)==PatchImage.Hash(payload),"missing owned bridge recovered");
            File.WriteAllBytes(module,new byte[]{1,2,3});bool refused=false;try{installer.Restore();}catch(IOException){refused=true;}
            Require(refused&&Hash(config)==installedConfig&&Hash(Path.Combine(root,"prototypeenginef.dll"))==patchedEngine,"foreign bridge blocks restore before mutations");File.WriteAllBytes(module,payload);
            string current=File.ReadAllText(config);File.WriteAllText(config,current.Replace("preserve-me","changed-while-game-running"),new UTF8Encoding(false));
            installer.Restore();Require(File.ReadAllText(config)==Fixture(null).Replace("preserve-me","changed-while-game-running"),"restore preserves unrelated settings changed since install");
            Require(Hash(Path.Combine(root,"prototypeenginef.dll"))==originalEngine&&Hash(Path.Combine(root,"prototypef.exe"))==originalLauncher&&!File.Exists(module),"restore returns original game files and removes helper");
            File.WriteAllText(config,Fixture(null),new UTF8Encoding(false));installer.Restore();Require(Hash(config)==originalConfig,"repeat restoration preserves exact configuration");
            using(var locked=new FileStream(config,FileMode.Open,FileAccess.Read,FileShare.Read)) {
                bool failed=false;try{installer.Install();}catch(IOException){failed=true;}
                Require(failed&&Hash(Path.Combine(root,"prototypeenginef.dll"))==originalEngine&&!File.Exists(module)&&Hash(config)==originalConfig,"failed configuration replace rolls back engine and bridge");
            }
            installer.Install();
            new BridgeInstaller(root,payload).Restore();
            string legacyPath=Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"version.dll");
            byte[] legacy=File.Exists(legacyPath)?File.ReadAllBytes(legacyPath):new byte[]{0x50,0x4d,0x30,0x31,0x32};
            new EarlyInstaller(root,legacy).Install();
            installer.Install();Require(!File.Exists(Path.Combine(root,"version.dll"))&&File.Exists(module),"0.1.2 unloaded proxy removed during upgrade");
            installer.Restore();Require(Hash(config)==originalConfig&&Hash(Path.Combine(root,"prototypeenginef.dll"))==originalEngine,"upgrade restoration is exact");
            string foreignProxy=Path.Combine(root,"version.dll");byte[] unrelated={7,8,9};File.WriteAllBytes(foreignProxy,unrelated);
            installer.Install();installer.Restore();Require(Hash(foreignProxy)==PatchImage.Hash(unrelated),"unrelated version.dll is preserved after retiring the legacy proxy");
            File.Delete(Path.Combine(root,"PrototypeModern.Backup","early-manifest.json"));
            installer.Install();installer.Restore();Require(Hash(foreignProxy)==PatchImage.Hash(unrelated),"new bridge does not claim another patch's version.dll without an ownership record");
            Console.WriteLine("No game launched; no original DLL loaded; real Steam configuration unchanged.");return 0;
        }catch(Exception error){Console.Error.WriteLine(error);return 1;}
    }
}
