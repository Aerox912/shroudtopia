"""Execute retained instructions in Unicorn, never launch the server process.

OS/Steam/log/lock boundaries are stubbed. This validates layout and transition
semantics only; it cannot establish callback delivery, threading or gameplay.
"""
import hashlib
import struct
import sys
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE
from unicorn.x86_const import *

image = Path(sys.argv[1]).read_bytes()
assert hashlib.sha256(image).hexdigest() == "d7baa768eb00aa895d9f5a1cd8747ccf66858c7b542f50983092b2e3261588ee"
pe = pefile.PE(data=image)
base = pe.OPTIONAL_HEADER.ImageBase
mapped = pe.get_memory_mapped_image()

def scenario(state, response, returned_handle=65, row_handle=65):
    uc = Uc(UC_ARCH_X86, UC_MODE_64)
    uc.mem_map(base, (len(mapped)+4095)&~4095)
    uc.mem_write(base, mapped)
    backend, event, stack = 0x400000000, 0x500000000, 0x600000000
    uc.mem_map(backend, 0x360000)
    uc.mem_map(event, 0x1000)
    uc.mem_map(stack, 0x10000)
    context = backend+0x348fc0
    row = context+0x68+0x1d8
    uc.mem_write(row, struct.pack('<HQ',row_handle,76561198000000042))
    uc.mem_write(row+0x98, bytes([state]))
    uc.mem_write(context+0x21c4, struct.pack('<H',9))
    uc.mem_write(event, bytes([19]))
    uc.mem_write(event+8, struct.pack('<QI',76561198000000042,response))
    uc.reg_write(UC_X86_REG_RDI,backend)
    uc.reg_write(UC_X86_REG_RBX,event)
    uc.reg_write(UC_X86_REG_RSP,stack+0x2000)
    uc.reg_write(UC_X86_REG_RBP,stack+0x2100)
    removed=[]
    def ret():
        sp=uc.reg_read(UC_X86_REG_RSP)
        uc.reg_write(UC_X86_REG_RIP,struct.unpack('<Q',uc.mem_read(sp,8))[0])
        uc.reg_write(UC_X86_REG_RSP,sp+8)
    def step(uc,address,size,user):
        rva=address-base
        if rva==0x9d6ce7:
            uc.emu_stop()
        elif rva==0x9c6650:
            assert uc.reg_read(UC_X86_REG_RCX)==context
            uc.mem_write(uc.reg_read(UC_X86_REG_RDX),struct.pack('<H',returned_handle))
            ret()
        elif rva in (0x55d400,0x573b20,0x56fe20):
            ret()
        elif rva==0x9c5060:
            removed.append(uc.reg_read(UC_X86_REG_RDX)&0xffff)
            ret()
    uc.hook_add(UC_HOOK_CODE,step)
    uc.emu_start(base+0x9d6b71,base+0x9d6ce7,count=3000)
    assert uc.reg_read(UC_X86_REG_RIP)==base+0x9d6ce7, 'instruction limit'
    return uc.mem_read(row+0x98,1)[0],struct.unpack('<H',uc.mem_read(context+0x21c4,2))[0],removed

assert scenario(1,0)==(2,10,[])
assert scenario(0,0)==(0,9,[])
assert scenario(2,0)==(2,9,[])
assert scenario(1,1)==(1,9,[65])
assert scenario(1,0,0)==(1,9,[])
assert scenario(1,0,129)==(1,9,[]) # same slot, different generation
print('PASS: retained auth branch and real peer lookup, six offline scenarios')
