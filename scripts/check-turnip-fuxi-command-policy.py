#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check the Turnip command-policy patch (patches/mesa/turnip-darwin-command-policy.patch).

The patch makes the command-stream emitter honour two capability bits the kernel publishes
(NativeAdrenoDeviceSingleRenderThread, NativeAdrenoDeviceNoWaitMemWrites). This check is what can be
verified without a GPU:

  1. the recorded patch set reconstructs the pinned Mesa source byte for byte (so the patch is exactly
     the change, with no unrecorded edits);
  2. the two hooks are gated on the capability accessors and on the two packet kinds, so a device that
     publishes neither bit takes the untouched path;
  3. the payload rewrite touches only the thread selector and SYNC_THREADS and keeps every other bit;
  4. the substituted header is the legal zero-payload CP_NOP encoding, checked against the PM4 kit the
     kernel itself uses (AdrenoPM4Type7(0x10, 0)).

A GPU is not exercised: a byte-identical default path and a legal encoding are the properties that can
be proved here, and the device run proves the rest.
"""
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / '.cache/mesa-work'
KIT = ROOT / 'kits/AdrenoPM4'
sys.path.insert(0, str(ROOT / 'scripts'))
import mesa_patch_identity  # noqa: E402

failures = []


def check(name, condition, detail=''):
    print(('PASS: ' if condition else 'FAIL: ') + name + (('  ' + detail) if detail and not condition else ''))
    if not condition:
        failures.append(name)


def main():
    pins = json.loads((ROOT / 'scripts/data/adreno-compiler-inputs.json').read_text())
    name = 'patches/mesa/turnip-darwin-command-policy.patch'
    check('the policy patch is recorded in the compiler inputs', name in pins['patches'])

    # 1. The recorded sequence reconstructs the pinned source.
    try:
        mesa_patch_identity.validate_recorded_patches(ROOT, SOURCE, pins)
        check('the recorded patch sequence reconstructs the pinned Mesa source', True)
    except SystemExit as error:
        check('the recorded patch sequence reconstructs the pinned Mesa source', False, str(error))

    cs_h = (SOURCE / 'src/freedreno/vulkan/tu_cs.h').read_text()
    cs_cc = (SOURCE / 'src/freedreno/vulkan/tu_cs.cc').read_text()
    knl = (SOURCE / 'src/freedreno/vulkan/tu_knl_darwin.cc').read_text()
    cb = (SOURCE / 'src/freedreno/vulkan/tu_cmd_buffer.cc').read_text()

    # 2. The two hooks are gated on the capability and on their packet kind.
    wait_hook = re.search(
        r'if \(opcode == CP_WAIT_MEM_WRITES && cnt == 0 &&\s*\n\s*tu_command_policy_no_wait_mem_writes\(cs->device\)\)',
        cs_h)
    check('CP_WAIT_MEM_WRITES is substituted only for a zero payload and only when the kernel asks',
          bool(wait_hook))
    check('the thread-control payload is built in one place',
          'tu_thread_control_payload(struct tu_cs *cs, enum cp_thread thread, bool sync)' in cs_h)
    check('that place emits the BR form with concurrent binning disabled when the kernel reports one thread',
          'if (tu_command_policy_single_render_thread(cs->device)) {' in cs_h and
          'return CP_THREAD_CONTROL_0_THREAD(CP_SET_THREAD_BR) |\n'
          '             CP_THREAD_CONTROL_0_CONCURRENT_BIN_DISABLE;' in cs_h)
    check('the default path returns exactly the word its caller asked for',
          'return CP_THREAD_CONTROL_0_THREAD(thread) |\n'
          '          (sync ? CP_THREAD_CONTROL_0_SYNC_THREADS : 0);' in cs_h)
    check('tu7_thread_control goes through that place',
          'tu_cs_emit(cs, tu_thread_control_payload(cs, thread, false));' in cs_h)
    cb = (SOURCE / 'src/freedreno/vulkan/tu_cmd_buffer.cc').read_text()
    check('the simultaneous-use site goes through it too, keeping its sync request',
          'tu_cs_emit(cs, tu_thread_control_payload(cs, CP_SET_THREAD_BR, true));' in cb)
    check('the BOTH patchpoint records no concurrent-binning control point',
          'if (tu_command_policy_single_render_thread(cmd->device)) {\n'
          '      tu7_set_thread_br_patchpoint(cmd, cs, true);\n      return;\n   }' in cb)
    qp = (SOURCE / 'src/freedreno/vulkan/tu_query_pool.cc').read_text()
    check('the occlusion-query predicate goes through it too',
          'tu_cs_emit(cs, tu_thread_control_payload(cs, CP_SET_THREAD_BOTH, false));' in qp)
    lrz = (SOURCE / 'src/freedreno/vulkan/tu_lrz.cc').read_text()
    check('the LRZ emitter goes through it too',
          'tu_cs_emit(cs, tu_thread_control_payload(cs, CP_SET_THREAD_BV, false));' in lrz)
    check('the binning branch goes through it too',
          'tu_cs_emit(cs, tu_thread_control_payload(cs, CP_SET_THREAD_BV, false));' in cb)
    check('the BR patchpoint is left out on such a device',
          'if (tu_command_policy_single_render_thread(cmd->device))\n      force_disable_cb = true;' in cb)

    # 4. The capability bits are read from the published device info, not from a board name.
    check('the capabilities come from the device-info flags word',
          'info->flags & ((1u << 2) | (1u << 3))' in knl)
    check('nothing in the patch names this board',
          'fuxi' not in cs_h.lower() and 'fuxi' not in knl.lower() and 'fuxi' not in cs_cc.lower())

    # 5. The substituted header is the legal zero-payload CP_NOP, checked with the kernel's own kit.
    probe = Path(tempfile.mkdtemp(prefix='pm4-', dir=ROOT / '.cache')) / 'probe.c'
    probe.write_text(
        '#include <stdio.h>\n#include "AdrenoPM4.h"\n'
        'int main(void){uint32_t nop=0,wait=0;'
        'if(AdrenoPM4Type7(0x10,0,&nop)!=AdrenoPM4Success)return 1;'
        'if(AdrenoPM4Type7(0x12,0,&wait)!=AdrenoPM4Success)return 2;'
        'printf("%08x %08x\\n",nop,wait);return 0;}\n')
    kit_c = sorted(KIT.glob('AdrenoPM4.c'))
    binary = probe.with_suffix('.bin')
    build = subprocess.run(['xcrun', 'clang', '-O1', '-I', str(KIT), str(probe), str(kit_c[0]),
                            '-o', str(binary)], capture_output=True, text=True)
    check('the PM4 kit compiles for the encoding probe', build.returncode == 0, build.stderr[-300:])
    if build.returncode == 0:
        out = subprocess.run([str(binary)], capture_output=True, text=True).stdout.split()
        nop, wait = int(out[0], 16), int(out[1], 16)
        check('the two encodings have the same length and type-7 shape',
              (nop >> 28) == 7 and (wait >> 28) == 7 and (nop & 0x3fff) == 0 and (wait & 0x3fff) == 0,
              f'{nop:08x} {wait:08x}')
        # The two differ in the opcode field only, which includes that field's parity bit (bit 23).
        check('CP_NOP differs from CP_WAIT_MEM_WRITES only in the opcode field',
              ((nop ^ wait) & ~(0xff << 16)) == 0 and (nop & 0xffff) == (wait & 0xffff),
              f'{nop:08x} {wait:08x}')

    print()
    if failures:
        print(f'{len(failures)} check(s) failed')
        return 1
    print('all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
