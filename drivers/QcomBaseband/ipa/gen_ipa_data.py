#!/usr/bin/env python3
"""Regenerate an IPA table header from a Linux tree.

usage: python3 -I gen_ipa_data.py LINUX_TREE STUB_DIR OUTPUT_HEADER [--version v4.2|v5.5] [--endpoints FILE]

Compiles drivers/net/ipa/reg/ipa_reg-vX.c, reg/gsi_reg-vY.c and data/ipa_data-vX.c on the host (STUB_DIR holds
minimal linux/*.h stand-ins) and links them with a generated program that indexes the real tables by the real enum
values of ipa_reg.h and gsi_reg.h. Register and field names are parsed from the sources; every number comes from the
compiled tables, so nothing is transcribed by hand.

The board may renumber endpoints: the generic table gives the AP-side modem endpoints 23 and 21, while some boards
use 24 and 22. --endpoints points at a file of "IPA_ENDPOINT_NAME = NUMBER" lines, applied by index into the
compiled endpoint array.
"""
import re, subprocess, sys, tempfile
from pathlib import Path

# The IPA data version selects three Linux sources and the three table symbols they define. The GSI register version
# is the one that GSI version shares its table with.
VERSIONS = {
    'v4.2': dict(reg='ipa_reg-v4.2.c', gsi='gsi_reg-v4.0.c', data='ipa_data-v4.2.c',
                 reg_table='ipa_regs_v4_2', gsi_table='gsi_regs_v4_0', data_table='ipa_data_v4_2',
                 guard='COACHZ_IPA_V42_H'),
    'v5.5': dict(reg='ipa_reg-v5.5.c', gsi='gsi_reg-v5.0.c', data='ipa_data-v5.5.c',
                 reg_table='ipa_regs_v5_5', gsi_table='gsi_regs_v5_0', data_table='ipa_data_v5_5',
                 guard='QCOM_IPA_V55_H'),
}
# The endpoint names the generated header publishes, in enum order.
ENDPOINT_NAMES = ('IPA_ENDPOINT_AP_COMMAND_TX', 'IPA_ENDPOINT_AP_LAN_RX', 'IPA_ENDPOINT_AP_MODEM_TX',
                   'IPA_ENDPOINT_AP_MODEM_RX', 'IPA_ENDPOINT_MODEM_COMMAND_TX', 'IPA_ENDPOINT_MODEM_LAN_TX',
                   'IPA_ENDPOINT_MODEM_LAN_RX', 'IPA_ENDPOINT_MODEM_AP_TX', 'IPA_ENDPOINT_MODEM_AP_RX',
                   'IPA_ENDPOINT_MODEM_DL_NLO_TX')
COMMIT = '6628e7d4c6b25c8d18f1345e3a20d21829aae7e4'
REG_MACRO = re.compile(r'^\s*REG(_STRIDE)?(_FIELDS)?\((\w+),\s*(\w+),\s*([^,)]+)(?:,\s*([^)]+))?\);', re.M)


def parse_arguments(argv):
    tree, stubs, out, version, endpoints = None, None, None, 'v4.2', None
    i = 0
    while i < len(argv):
        value = argv[i]
        if value == '--version':
            version = argv[i + 1]; i += 2; continue
        if value == '--endpoints':
            endpoints = Path(argv[i + 1]); i += 2; continue
        if tree is None:
            tree = value
        elif stubs is None:
            stubs = value
        elif out is None:
            out = value
        else:
            raise SystemExit('unexpected argument: ' + value)
        i += 1
    if not tree or not stubs or not out:
        raise SystemExit(__doc__)
    if version not in VERSIONS:
        raise SystemExit('unknown IPA version: ' + version + ' (have ' + ', '.join(VERSIONS) + ')')
    return Path(tree), Path(stubs), Path(out), VERSIONS[version], endpoints


def registers(path):
    text = path.read_text()
    fields = {}
    for m in re.finditer(r'static const u32 reg_(\w+)_fmask\[\] = \{(.*?)\n\};', text, re.S):
        fields[m.group(1)] = re.findall(r'\[(\w+)\]\s*=', m.group(2))
    regs = []
    for m in REG_MACRO.finditer(text):
        name, lower, has_fields = m.group(3), m.group(4), bool(m.group(2))
        regs.append((name, fields.get(lower, []) if has_fields else []))
    return regs


def board_endpoints(path):
    """Read "IPA_ENDPOINT_NAME = NUMBER" lines into {enum index: number}."""
    if path is None:
        return {}
    result = {}
    for line in path.read_text().splitlines():
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        name, _, value = line.partition('=')
        name, value = name.strip(), value.strip()
        if name not in ENDPOINT_NAMES:
            raise SystemExit('unknown endpoint name: ' + name)
        result[ENDPOINT_NAMES.index(name)] = int(value, 0)
    return result


tree, stubs, out, spec, endpoints_path = parse_arguments(sys.argv[1:])
ipa = tree / 'drivers/net/ipa'
overrides = board_endpoints(endpoints_path)
ipa_regs, gsi_regs = registers(ipa / ('reg/' + spec['reg'])), registers(ipa / ('reg/' + spec['gsi']))
prog = ['#include <stdio.h>', '#include <stdint.h>', '#include "ipa_reg.h"', '#include "gsi_reg.h"', '#include "ipa_data.h"', '#include "ipa_mem.h"',
        'static void reg_out(const char *p, const char *n, const struct regs *rs, unsigned id, unsigned nf, const unsigned *fi, const char *const *fn) {',
        '  const struct reg *r = rs->reg[id];',
        '  if (!r) { printf("/* %s_%s: not on this version */\\n", p, n); return; }',
        '  printf("#define %s_%s 0x%05x\\n", p, n, r->offset);',
        '  if (r->stride) printf("#define %s_%s_STRIDE 0x%x\\n", p, n, r->stride);',
        '  for (unsigned i = 0; i < nf; i++) if (fi[i] < r->fcount && r->fmask[fi[i]]) printf("#define %s_%s_%s 0x%08x\\n", p, n, fn[i], r->fmask[fi[i]]);',
        '}', 'int main(void) {']
for prefix, regs, table in (('CIPA', ipa_regs, spec['reg_table']), ('CGSI', gsi_regs, spec['gsi_table'])):
    for name, fields in regs:
        ids = ', '.join(fields) or '0'
        names = ', '.join('"%s"' % f for f in fields) or '""'
        prog.append('  { static const unsigned fi[] = { %s }; static const char *const fn[] = { %s };'
                    ' reg_out("%s", "%s", &%s, %s, %d, fi, fn); }' % (ids, names, prefix, name, table, name, len(fields)))
prog += [
    '  const struct ipa_data *d = &' + spec['data_table'] + ';',
    '  printf("\\n#define CIPA_VERSION %u\\n#define CIPA_BACKWARD_COMPAT 0x%x\\n#define CIPA_MODEM_ROUTE_COUNT %u\\n", d->version, d->backward_compat, d->modem_route_count);',
    '  printf("#define CIPA_CORE_CLOCK_RATE %u\\n#define CIPA_SMEM_SIZE 0x%x\\n", d->power_data->core_clock_rate, d->mem_data->smem_size);',
    '  printf("\\nstatic const struct cipa_qsb cipa_qsb[] = {\\n");',
    '  for (unsigned i = 0; i < d->qsb_count; i++) printf("    { %u, %u, %u },\\n", d->qsb_data[i].max_writes, d->qsb_data[i].max_reads, d->qsb_data[i].max_reads_beats);',
    '  printf("};\\n\\n/* ee, channel, endpoint, toward_ipa, tre, event, tlv, filter, resource group, checksum, qmap, aggregation,\\n'
    '   status, dma mode, dma endpoint, tx seq type, tx seq rep type, tx status endpoint, rx buffer size, pad align,\\n'
    '   aggr time limit, aggr hard limit, aggr close eof, holb drop; indexed by enum ipa_endpoint_name */\\n");',
    '  printf("static const struct cipa_endpoint cipa_endpoints[%u] = {\\n", d->endpoint_count);',
    '  for (unsigned i = 0; i < d->endpoint_count; i++) { const struct ipa_gsi_endpoint_data *e = &d->endpoint_data[i];',
    '    const struct ipa_endpoint_config *c = &e->endpoint.config;',
    '    if (!e->channel.tre_count && !e->endpoint_id && !e->channel_id) { printf("    { 0 },\\n"); continue; }',
    '    uint32_t endpoint_id = e->endpoint_id;',
    '    switch (i) {']
for index, number in sorted(overrides.items()):
    prog.append('      case %d: endpoint_id = %d; break;' % (index, number))
prog += [
    '      default: break; }',
    '    printf("    { %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, ", e->ee_id, e->channel_id, endpoint_id, e->toward_ipa,',
    '      e->channel.tre_count, e->channel.event_count, e->channel.tlv_count, e->endpoint.filter_support, c->resource_group, c->checksum,',
    '      c->qmap, c->aggregation, c->status_enable, c->dma_mode, c->dma_endpoint);',
    '    if (e->toward_ipa) printf("%u, %u, %u, 0, 0, 0, 0, 0, 0 },\\n", c->tx.seq_type, c->tx.seq_rep_type, c->tx.status_endpoint);',
    '    else printf("0, 0, 0, %u, %u, %u, %u, %u, %u },\\n", c->rx.buffer_size, c->rx.pad_align, c->rx.aggr_time_limit, c->rx.aggr_hard_limit,',
    '      c->rx.aggr_close_eof, c->rx.holb_drop); }',
    '  printf("};\\n\\n#define CIPA_RSRC_GROUP_SRC_COUNT %u\\n#define CIPA_RSRC_GROUP_DST_COUNT %u\\n", d->resource_data->rsrc_group_src_count, d->resource_data->rsrc_group_dst_count);',
    '  printf("/* per resource type: { min, max } for each group */\\nstatic const uint32_t cipa_resource_src[][%d][2] = {\\n", IPA_RESOURCE_GROUP_MAX);',
    '  for (unsigned i = 0; i < d->resource_data->resource_src_count; i++) { printf("    {"); for (unsigned g = 0; g < IPA_RESOURCE_GROUP_MAX; g++)',
    '    printf(" { %u, %u },", d->resource_data->resource_src[i].limits[g].min, d->resource_data->resource_src[i].limits[g].max); printf(" },\\n"); }',
    '  printf("};\\nstatic const uint32_t cipa_resource_dst[][%d][2] = {\\n", IPA_RESOURCE_GROUP_MAX);',
    '  for (unsigned i = 0; i < d->resource_data->resource_dst_count; i++) { printf("    {"); for (unsigned g = 0; g < IPA_RESOURCE_GROUP_MAX; g++)',
    '    printf(" { %u, %u },", d->resource_data->resource_dst[i].limits[g].min, d->resource_data->resource_dst[i].limits[g].max); printf(" },\\n"); }',
    '  printf("};\\n");',
    '  printf("\\n/* enum ipa_endpoint_name */\\n#define CIPA_EP_AP_COMMAND_TX %u\\n#define CIPA_EP_AP_LAN_RX %u\\n#define CIPA_EP_AP_MODEM_TX %u\\n#define CIPA_EP_AP_MODEM_RX %u\\n#define CIPA_EP_COUNT %u\\n",',
    '    IPA_ENDPOINT_AP_COMMAND_TX, IPA_ENDPOINT_AP_LAN_RX, IPA_ENDPOINT_AP_MODEM_TX, IPA_ENDPOINT_AP_MODEM_RX, IPA_ENDPOINT_COUNT);',
    '  return 0; }']
with tempfile.TemporaryDirectory() as d:
    d = Path(d)
    (d / 'dump.c').write_text('\n'.join(prog) + '\n')
    sources = [d / 'dump.c', ipa / ('reg/' + spec['reg']), ipa / ('reg/' + spec['gsi']), ipa / ('data/' + spec['data'])]
    subprocess.run(['cc', '-std=gnu11', '-w', '-I', str(stubs), '-I', str(ipa), '-I', str(ipa / 'reg'), *map(str, sources), '-o', str(d / 'dump')], check=True)
    body = subprocess.run([str(d / 'dump')], check=True, capture_output=True, text=True).stdout
out.write_text('/* SPDX-License-Identifier: BSD-2-Clause */\n'
               '/* Generated by gen_ipa_data.py from Linux %s drivers/net/ipa/reg/%s, reg/%s\n'
               '   and data/%s. Do not edit; regenerate. Offsets are from the "ipa-reg" and "gsi"\n'
               '   bases; *_STRIDE is the per-instance step of parameterized registers; field values are masks. */\n'
               '#ifndef %s\n#define %s\n#include <stdint.h>\n\n'
               'struct cipa_qsb { uint8_t max_writes, max_reads, max_reads_beats; };\n'
               'struct cipa_endpoint { uint8_t ee, channel, endpoint, toward_ipa; uint16_t tre_count, event_count; uint8_t tlv_count, filter,\n'
               '    resource_group, checksum, qmap, aggregation, status_enable, dma_mode, dma_endpoint, tx_seq_type, tx_seq_rep_type,\n'
               '    tx_status_endpoint; uint32_t rx_buffer_size, rx_pad_align, rx_aggr_time_limit; uint8_t rx_aggr_hard_limit,\n'
               '    rx_aggr_close_eof, rx_holb_drop; };\n\n%s\n#endif\n' % (COMMIT, spec['reg'], spec['gsi'], spec['data'], spec['guard'], spec['guard'], body))
print(out, body.count('#define'), 'defines')
