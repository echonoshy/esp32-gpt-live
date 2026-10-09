#!/usr/bin/env python3
"""Host C codec cross-check against independent Python audioop reference vectors."""
import ctypes as c
import json
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
class State(c.Structure):
    _fields_ = [('predictor', c.c_int), ('index', c.c_int)]
with tempfile.TemporaryDirectory(prefix='adpcm-check-') as td:
    shared = str(Path(td) / 'adpcm.so')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-shared', '-fPIC', '-O2', str(root/'firmware/main/adpcm.c'), '-o', shared], check=True)
    lib = c.CDLL(shared)
    lib.adpcm_encode.argtypes = [c.POINTER(State), c.POINTER(c.c_int16), c.POINTER(c.c_uint8)]
    lib.adpcm_decode.argtypes = [c.POINTER(c.c_uint8), c.c_size_t, c.POINTER(c.c_int16)]
    lib.adpcm_decode.restype = c.c_bool
    for v in json.loads((root/'scripts/fixtures/adpcm.json').read_text()):
        state = State(*v['initial'])
        pcm = (c.c_int16*480).from_buffer_copy(bytes.fromhex(v['pcm']))
        encoded = (c.c_uint8*244)()
        decoded = (c.c_int16*480)()
        lib.adpcm_encode(c.byref(state), pcm, encoded)
        assert bytes(encoded).hex() == v['encoded']
        assert [state.predictor, state.index] == v['final']
        assert lib.adpcm_decode(encoded, 244, decoded)
        assert bytes(decoded).hex() == v['decoded']
        encoded[2] = 89
        assert not lib.adpcm_decode(encoded, 244, decoded)
        assert not lib.adpcm_decode(encoded, 243, decoded)
print('PASS: C codec matches Python audioop for silence, smooth audio, ramps, clipping; invalid packets rejected')
