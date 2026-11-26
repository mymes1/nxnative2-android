# spv_android.py's --gpu Mali target: NonWritable on the vertex pipeline's read-only storage resources at pack time,
# the pass dm/android/spv_readonly.h implements for the host at shader-module load (a GPU without
# vertexPipelineStoresAndAtomics: the Mali-G57 MC2, the Mali-G76). spv_readonly_words is that C function ported to
# Python word for word; these tests are its unit tests, and — with the Android NDK — a cross-check of the port against
# spv_readonly.h itself over the same modules (the C side compiled by the NDK's clang, as test_spv_readonly.py does) and
# an end-to-end run of spv_android.py --gpu mali-g57-mc2 over a shader assembled with the NDK's spirv-as.
import ctypes
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/android/tools'))
import spv_android  # noqa: E402


def ndk():
    for c in (os.environ.get('ANDROID_NDK_ROOT'), *(Path(s) / 'ndk' for s in (os.environ.get('ANDROID_HOME'), os.environ.get('ANDROID_SDK_ROOT'),
              Path(os.environ.get('LOCALAPPDATA', '')) / 'Android' / 'Sdk') if s)):
        if c and Path(c).is_dir():
            vs = sorted(Path(c).iterdir()) if not (Path(c) / 'shader-tools').is_dir() else [Path(c)]
            for v in reversed(vs):
                if (v / 'shader-tools').is_dir(): return v
    return None


NDK = ndk()
HOST = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64' if os.uname().sysname == 'Linux' else 'darwin-x86_64'
EXE = '.exe' if os.name == 'nt' else ''

# ---- A tiny SPIR-V assembler for the fixtures (the NDK's spirv-as assembles the same modules for the cross-check) ----
class Asm:
    def __init__(self):
        self.next = 1
        self.ids = {}
        self.words = [0x07230203, 0, 0, 0, 0]   # magic, version, generator, bound, schema

    def id(self, name):
        if name not in self.ids: self.ids[name] = self.next; self.next += 1
        return self.ids[name]

    def ins(self, op, *ops):
        self.words.append((len(ops) + 1) << 16 | op)
        self.words.extend(ops)

    def name(self, text):   # a literal string: its words, length-prefixed
        b = text.encode() + b'\0'
        ws = [struct.unpack('<I', b[i:i + 4].ljust(4, b'\0'))[0] for i in range(0, len(b), 4)]
        ws[0] = (len(ws) << 16) | (ws[0] & 0xffff)
        return ws

    def bytes(self):
        self.words[3] = self.next
        return struct.pack('<%dI' % len(self.words), *self.words)


def vs_fixture():
    """The vertex shader of test_spv_readonly.py's fixture: a read-only buffer (%ro) and a written one (%rw), both
    Uniform BufferBlocks; only %ro may be decorated NonWritable"""
    a = Asm()
    void, fn, f32, v4, arr, buf, pbuf, ro, rw, pv4, pout, pos = (a.id(x) for x in
        ('void', 'fn', 'f32', 'v4', 'arr', 'buf', 'pbuf', 'ro', 'rw', 'pv4', 'pout', 'pos'))
    i32, c0, main, lab, ac, v, ac2, b = (a.id(x) for x in ('i32', 'c0', 'main', 'lab', 'ac', 'v', 'ac2', 'b'))
    a.ins(17, 0x20000000)                      # OpCapability Shader
    a.ins(14, 0, 1)                            # OpMemoryModel Logical GLSL450
    a.ins(15, 0, main, *a.name('main'), pos, 0)   # OpEntryPoint Vertex %main "main" %pos
    a.ins(71, pos, 11, 0)                      # OpDecorate %pos BuiltIn Position
    a.ins(71, ro, 34, 0); a.ins(71, ro, 33, 0)     # %ro DescriptorSet 0, Binding 0
    a.ins(71, rw, 34, 0); a.ins(71, rw, 33, 1)     # %rw DescriptorSet 0, Binding 1
    a.ins(71, buf, 3)                          # OpDecorate %buf BufferBlock
    a.ins(72, buf, 0, 35, 0)                   # OpMemberDecorate %buf 0 Offset 0
    a.ins(71, arr, 6, 16)                      # OpDecorate %arr ArrayStride 16
    a.ins(19, void)                            # OpTypeVoid
    a.ins(33, void)                            # OpTypeFunction %void
    a.ins(22, f32, 32)                              # OpTypeFloat 32
    a.ins(23, v4, f32, 4)                      # OpTypeVector %v4 %f32 4
    a.ins(29, v4)                              # OpTypeRuntimeArray %v4
    a.ins(30, arr)                             # OpTypeStruct %arr
    a.ins(32, pbuf, 2, buf)                    # OpTypePointer %pbuf Uniform %buf
    a.ins(59, pbuf, ro, 2)                     # OpVariable %pbuf %ro Uniform
    a.ins(59, pbuf, rw, 2)                     # OpVariable %pbuf %rw Uniform
    a.ins(32, pv4, 2, v4)                      # OpTypePointer %pv4 Uniform %v4
    a.ins(32, pout, 3, v4)                     # OpTypePointer %pout Output %v4
    a.ins(59, pout, pos, 3)                    # OpVariable %pout %pos Output
    a.ins(21, i32, 32, 1)                           # OpTypeInt 32 1
    a.ins(43, i32, c0, 0)                      # OpConstant %i32 0
    a.ins(54, void, 0, fn)                     # OpFunction %void None %fn
    a.ins(248, lab)                            # OpLabel
    a.ins(65, pv4, ac, ro, c0, c0)             # OpAccessChain %pv4 %ac %ro %c0 %c0
    a.ins(61, v4, v, ac)                       # OpLoad %v4 %v
    a.ins(62, pos, v)                          # OpStore %pos %v
    a.ins(65, pv4, ac2, rw, c0, c0)            # OpAccessChain %pv4 %ac2 %rw %c0 %c0
    a.ins(62, ac2, v)                          # OpStore %ac2 %v
    a.ins(253)                                 # OpReturn
    a.ins(56)                                  # OpFunctionEnd
    return a.bytes()


def decorated(data):   # the ids decorated NonWritable
    w = struct.unpack(f'<{len(data) // 4}I', data)
    out, i = set(), 5
    while i < len(w):
        if w[i] & 0xffff == 71 and w[i] >> 16 == 3 and w[i + 2] == 24: out.add(w[i + 1])
        i += w[i] >> 16
    return out


class SpvReadonlyWords(unittest.TestCase):
    """spv_readonly_words: the port's own behaviour, without the NDK"""

    def test_read_only_buffer_decorated_written_one_left(self):
        w = list(struct.unpack('<%dI' % (len(vs_fixture()) // 4), vs_fixture()))
        out, written, added = spv_android.spv_readonly_words(w)
        self.assertIsNotNone(out)
        self.assertEqual((written, added), (1, 1))
        self.assertEqual(len(decorated(struct.pack('<%dI' % len(out), *out))), 1)

    def test_fragment_shader_left_alone(self):
        a = Asm()
        void, fn, f32, v4, pout, pos, i32, c0, main, lab = (a.id(x) for x in ('void', 'fn', 'f32', 'v4', 'pout', 'pos', 'i32', 'c0', 'main', 'lab'))
        a.ins(17, 0x20000000)
        a.ins(14, 0, 1)
        a.ins(15, 4, main, *a.name('main'), pos, 0)   # Fragment (4: not a vertex-pipeline stage)
        a.ins(16, main, 0)                            # OpExecutionMode OriginUpperLeft
        a.ins(71, pos, 0, 0)                          # OpDecorate %pos Location 0
        a.ins(19, void); a.ins(33, void); a.ins(22, f32, 32); a.ins(23, v4, f32, 4)
        a.ins(32, pout, 3, v4)
        a.ins(59, pout, pos, 3)
        a.ins(21, i32, 32, 1); a.ins(43, i32, c0, 0)
        a.ins(54, void, 0, fn); a.ins(248, lab); a.ins(253); a.ins(56)
        out, written, added = spv_android.spv_readonly_words(list(struct.unpack('<%dI' % (len(a.bytes()) // 4), a.bytes())))
        self.assertEqual((out, written, added), (None, 0, 0))

    def test_already_decorated_and_idempotent(self):
        w = list(struct.unpack('<%dI' % (len(vs_fixture()) // 4), vs_fixture()))
        out, _, _ = spv_android.spv_readonly_words(w)
        out2, written2, added2 = spv_android.spv_readonly_words(out)
        self.assertEqual((out2, written2, added2), (None, 1, 0))   # the decoration is there: nothing to add (the written one still counts)

    def test_compute_stage_is_not_a_vertex_pipeline(self):
        a = Asm()   # a compute shader declaring a storage buffer it reads: the rule does not apply to it
        void, fn, i32, sb, sbp, buf, arr, main, lab, ac, v = (a.id(x) for x in ('void', 'fn', 'i32', 'sb', 'sbp', 'buf', 'arr', 'main', 'lab', 'ac', 'v'))
        a.ins(17, 0x20000000 | (1 << 18))            # Shader + StorageBuffer
        a.ins(14, 0, 1)
        a.ins(15, 5, main, *a.name('main'))          # OpEntryPoint GLCompute (5: not a vertex-pipeline stage)
        a.ins(71, sb, 34, 0); a.ins(71, sb, 33, 0)
        a.ins(71, buf, 3); a.ins(72, buf, 0, 35, 0); a.ins(71, arr, 6, 16)
        a.ins(19, void); a.ins(33, void); a.ins(21, i32, 32, 1)
        a.ins(29, i32); a.ins(30, arr)
        a.ins(32, sbp, 12, buf)                      # OpTypePointer StorageBuffer %buf
        a.ins(59, sbp, sb, 12)
        a.ins(54, void, 0, fn); a.ins(248, lab)
        a.ins(61, i32, v, ac)                        # OpLoad: the buffer is read, not written
        a.ins(253); a.ins(56)
        out, written, added = spv_android.spv_readonly_words(list(struct.unpack('<%dI' % (len(a.bytes()) // 4), a.bytes())))
        self.assertEqual((out, written, added), (None, 0, 0))


@unittest.skipIf(NDK is None, 'no Android NDK')
class SpvReadonlyCrossCheck(unittest.TestCase):
    """spv_readonly_words against spv_readonly.h itself, over modules assembled by the NDK's spirv-as"""

    @classmethod
    def setUpClass(cls):
        assert NDK is not None
        cls.tmp = Path(tempfile.mkdtemp())
        tools = NDK / 'shader-tools' / HOST
        cls.spv_as, cls.spv_val = tools / f'spirv-as{EXE}', tools / f'spirv-val{EXE}'
        bin_ = NDK / 'toolchains/llvm/prebuilt' / HOST / 'bin'
        src = cls.tmp / 'r.c'
        export = '__declspec(dllexport)' if os.name == 'nt' else '__attribute__((visibility("default")))'
        src.write_text(f'#include "spv_readonly.h"\n{export} size_t run(const uint32_t *w, size_t n, uint32_t *s, uint32_t *o, size_t cap, int *wr) {{ return spv_readonly(w, n, s, o, cap, wr); }}\n')
        env = dict(os.environ, MSYS_NO_PATHCONV='1')
        if os.name == 'nt':   # no C library: a DLL linked by lld alone
            subprocess.run([bin_ / 'clang.exe', '--target=x86_64-pc-windows-msvc', '-ffreestanding', '-nostdlib', '-O1', '-Wall', '-I', REPO / 'dm/android', '-c', src, '-o', cls.tmp / 'r.obj'], check=True)
            subprocess.run([bin_ / 'ld.lld.exe', '-flavor', 'link', '-dll', '-noentry', f'-out:{cls.tmp / "r.dll"}', str(cls.tmp / 'r.obj')], check=True, env=env)
            cls.lib = ctypes.CDLL(str(cls.tmp / 'r.dll'))
        else:
            subprocess.run([bin_ / 'clang', '-shared', '-fPIC', '-O1', '-I', REPO / 'dm/android', src, '-o', cls.tmp / 'r.so'], check=True)
            cls.lib = ctypes.CDLL(str(cls.tmp / 'r.so'))
        cls.lib.run.restype = ctypes.c_size_t

    def c_run(self, data):
        n = len(data) // 4
        w = (ctypes.c_uint32 * n).from_buffer_copy(data)
        bound = w[3]
        s = (ctypes.c_uint32 * (2 * bound))()
        o = (ctypes.c_uint32 * (n + 3 * bound))()
        wr = ctypes.c_int()
        k = self.lib.run(w, n, s, o, n + 3 * bound, ctypes.byref(wr))
        return (bytes(o)[:4 * k] if k else None), wr.value

    def asm(self, text, name):
        p = self.tmp / f'{name}.spvasm'; p.write_text(text)
        subprocess.run([self.spv_as, '--target-env', 'vulkan1.1', p, '-o', self.tmp / f'{name}.spv'], check=True)
        return (self.tmp / f'{name}.spv').read_bytes()

    VS_TEXT = '''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %pos
OpDecorate %pos BuiltIn Position
OpDecorate %ro DescriptorSet 0
OpDecorate %ro Binding 0
OpDecorate %rw DescriptorSet 0
OpDecorate %rw Binding 1
OpDecorate %buf BufferBlock
OpMemberDecorate %buf 0 Offset 0
OpDecorate %arr ArrayStride 16
%void = OpTypeVoid
%fn = OpTypeFunction %void
%f32 = OpTypeFloat 32
%v4 = OpTypeVector %f32 4
%arr = OpTypeRuntimeArray %v4
%buf = OpTypeStruct %arr
%pbuf = OpTypePointer Uniform %buf
%ro = OpVariable %pbuf Uniform
%rw = OpVariable %pbuf Uniform
%pv4 = OpTypePointer Uniform %v4
%pout = OpTypePointer Output %v4
%pos = OpVariable %pout Output
%i32 = OpTypeInt 32 1
%c0 = OpConstant %i32 0
%main = OpFunction %void None %fn
%l = OpLabel
%a = OpAccessChain %pv4 %ro %c0 %c0
%v = OpLoad %v4 %a
OpStore %pos %v
%b = OpAccessChain %pv4 %rw %c0 %c0
OpStore %b %v
OpReturn
OpFunctionEnd
'''

    def test_port_agrees_with_c_on_the_fixture(self):
        data = self.asm(self.VS_TEXT, 'vs')
        c_out, c_wr = self.c_run(data)
        py_out, py_wr, py_add = spv_android.spv_readonly_words(list(struct.unpack('<%dI' % (len(data) // 4), data)))
        self.assertIsNotNone(c_out)
        self.assertEqual((py_out is None, py_wr, py_add), (False, c_wr, len(decorated(c_out))))
        # py_out is a list of words, c_out is the same words as bytes: pack before comparing
        self.assertEqual(struct.pack('<%dI' % len(py_out), *py_out), c_out)

    def test_port_output_is_valid_vulkan11(self):
        data = self.asm(self.VS_TEXT, 'vs')
        out, written, added = spv_android.spv_readonly_words(list(struct.unpack('<%dI' % (len(data) // 4), data)))
        p = self.tmp / 'py.spv'; p.write_bytes(struct.pack('<%dI' % len(out), *out))
        r = subprocess.run([self.spv_val, '--target-env', 'vulkan1.1', p], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


@unittest.skipIf(NDK is None, 'no Android NDK')
class SpvAndroidGpu(unittest.TestCase):
    """spv_android.py --gpu mali-g57-mc2 end to end, over a shader assembled with the NDK's spirv-as"""

    @classmethod
    def setUpClass(cls):
        assert NDK is not None
        cls.tmp = Path(tempfile.mkdtemp())
        cls.spv_as = NDK / 'shader-tools' / HOST / f'spirv-as{EXE}'

    def run_spv_android(self, gpu, text, name):
        src, dst = self.tmp / f'{name}-src', self.tmp / f'{name}-{gpu}'
        src.mkdir(exist_ok=True); dst.mkdir(exist_ok=True)
        asm = self.tmp / f'{name}.spvasm'; asm.write_text(text)
        spv = self.tmp / f'{name}.spv'
        subprocess.run([self.spv_as, '--target-env', 'vulkan1.1', asm, '-o', spv], check=True)
        (src / f'{name}.spv').write_bytes(spv.read_bytes())
        (src / f'{name}.res.json').write_text(json.dumps({'stage': 'vs'}), encoding='utf-8')
        r = subprocess.run([sys.executable, str(REPO / 'dm/android/tools/spv_android.py'), str(src), str(dst), '--gpu', gpu],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return dst / f'{name}.spv', json.loads((dst / f'{name}.res.json').read_text(encoding='utf-8'))

    def test_mali_target_decorates_the_vertex_shader(self):
        spv, res = self.run_spv_android('mali-g57-mc2', SpvReadonlyCrossCheck.VS_TEXT, 'vs')
        self.assertEqual(res['gpu'], 'mali-g57-mc2')
        self.assertEqual(len(decorated(spv.read_bytes())), 1)   # %ro; %rw is written
        self.assertTrue(any('NonWritable' in n for n in res.get('notes', [])))

    def test_default_target_leaves_it_alone(self):
        spv, res = self.run_spv_android('default', SpvReadonlyCrossCheck.VS_TEXT, 'vs')
        self.assertEqual(res['gpu'], 'default')
        self.assertEqual(decorated(spv.read_bytes()), set())


if __name__ == '__main__':
    unittest.main()
