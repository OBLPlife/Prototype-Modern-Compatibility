using System;
using System.IO;
using System.Linq;
using System.Text;

namespace PrototypeModern {
    public static class LauncherPatch {
        internal static bool HasStartupReceive(byte[] bytes) {
            var pe=new PeImage(bytes);
            // Only the launcher's receive import is relevant to the optional startup helper.
            return pe.ImportOrdinal("WS2_32.dll",16)==0x3184;
        }
        public static void ValidateOriginal(byte[] original) {
            var pe=new PeImage(original);
            if(pe.ImageBase!=0x400000)throw new InvalidDataException(InstallerText.LauncherLayout);
            var text=pe.Sections.SingleOrDefault(s=>s.Name==".text");
            // Local encrypted code-section fingerprint plus imports and layout.
            // The full executable hash is only used for backup ownership.
            if(text==null||text.Rva!=0x1000||text.VirtualSize!=0x1c3c||text.RawSize!=0x2000||
                PatchImage.Hash(original.Skip(text.Raw).Take(text.RawSize).ToArray())!=
                "c5821a5d520915b6bcedf6a68cebb8a0da0a7465b68010032eaeb780ae94ac6d")
                throw new InvalidDataException(InstallerText.LauncherSignature);
            // The observed original launcher already has an Authenticode block.
            // Keep its bytes and the Steam entry/.bind intact. Any import edit
            // changes Authenticode's digest; restoring the backup restores it.
            int certificate=pe.I32(pe.Optional+96+4*8),certificateSize=pe.I32(pe.Optional+100+4*8);
            if(certificate!=0)pe.Bounds(certificate,certificateSize);
            int[] ordinals={16,115,23,116,3,11,9,4};
            for(int i=0;i<ordinals.Length;++i)
                if(pe.ImportOrdinal("WS2_32.dll",ordinals[i])!=0x3184+i*4)
                    throw new InvalidDataException(InstallerText.ImportLayout);
            int at=pe.Offset(pe.I32(pe.Optional+104),20),nameRva=0;
            for(int i=0;i<256;++i,at+=20) {
                int name=pe.I32(at+12);if(name==0)break;
                if(String.Equals(pe.CString(name),"WS2_32.dll",StringComparison.OrdinalIgnoreCase)) {
                    if(nameRva!=0)throw new InvalidDataException(InstallerText.ImportUnique);
                    nameRva=name;
                }
            }
            if(nameRva==0||pe.CString(nameRva).Length!=10)throw new InvalidDataException(InstallerText.ImportName);
        }
        public static byte[] Create(byte[] original,out string details) {
            details=null;
            throw new NotSupportedException(InstallerText.RetiredTechnique);
        }
    }
}
