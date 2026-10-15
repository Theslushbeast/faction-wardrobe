# Builds FactionWardrobe.esp: 128 empty outfit records (0x800-0x87F) that the DLL fills at runtime. ESL-flagged.
import struct,os
def sub(t,d): return t+struct.pack('<H',len(d))+d
def rec(t,fid,body): return t+struct.pack('<III',len(body),0,fid)+struct.pack('<HHHH',0,0,44,0)+body
recs=b''.join(rec(b'OTFT',0x800+i,sub(b'EDID',b'FactionWardrobe_Slot%03d\0'%i)) for i in range(128))
grp=b'GRUP'+struct.pack('<I',24+len(recs))+b'OTFT'+struct.pack('<IHHHH',0,0,0,0,0)+recs
hdr=sub(b'HEDR',struct.pack('<fII',1.71,128,0x880))+sub(b'CNAM',b'Clyde\0')+sub(b'SNAM',b'Outfit slots for Faction Wardrobe. Filled by FactionWardrobe.dll at runtime.\0')
tes4=b'TES4'+struct.pack('<III',len(hdr),0x200,0)+struct.pack('<HHHH',0,0,44,0)+hdr
out=os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','esp','FactionWardrobe.esp')
open(out,'wb').write(tes4+grp); print(out,len(tes4+grp))
