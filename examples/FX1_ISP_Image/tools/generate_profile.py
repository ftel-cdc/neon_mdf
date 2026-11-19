"""Translate the application-owned tuning profile into Neon configuration structures.

No Reference driver or register writes are compiled into the application.
Reject new/unmapped registers so tuning changes cannot be silently dropped.
"""
from pathlib import Path
import hashlib
import argparse

def require(condition, message):
    if not condition:
        raise ValueError(message)

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--profile', type=Path, default=root / 'profile/profile.csrw')
parser.add_argument('--output', type=Path, default=root / 'profile/profile.h')
args = parser.parse_args()
source = args.profile

pairs = []
pairs_by_name = {}
for line_number, line in enumerate(source.read_text().splitlines(), 1):
    fields = line.split('#', 1)[0].split()
    if not fields:
        continue
    location = f'{source}:{line_number}'
    require(len(fields) == 2,
            f'{location}: expected register and value (2 fields), got {len(fields)}')
    name, value = fields
    try:
        number = int(value, 0)
    except ValueError:
        raise ValueError(f'{location}: {name}: invalid integer {value!r}') from None
    require(0 <= number <= 0xffffffff,
            f'{location}: {name}: expected uint32 value, got {value}')
    pairs.append((name, number))
    pairs_by_name.setdefault(name, []).append(number)
values = dict(pairs)
used = set()
lines = []
field_masks = {}

def get(name):
    require(name in values, f'{source}: required register not found: {name}')
    used.add(name)
    return values[name]

def put(field, value):
    lines.append(f'    cfg->{field} = {value};')

def scalar(field, name, shift=0, mask=0xffffffff):
    field_masks[name] = field_masks.get(name, 0) | (mask << shift)
    put(field, (get(name) >> shift) & mask)

def array(name, field, count):
    used.add(name)
    data = pairs_by_name.get(name, [])
    require(len(data) == count,
            f'{name}: expected {count} entries, got {len(data)}')
    lines.append(f'    static const uint32_t {field}[{count}] = {{\n        ' +
                 ', '.join(f'0x{v:x}u' for v in data) + '\n    };')
    return data

scalar('ifmt.bayer', 'COMMON_BAYER')
scalar('blc.enable', 'BLC_CTRL', mask=1)
require(get('BLC_GAIN_SEL') & 7 == 0, 'Profile schema configures BLC gain group 0 only')
scalar('blc.gain_level', 'BLC_GAIN_SEL', mask=7)
scalar('blc.range_scale_en', 'BLC_GAIN_SEL', 8, 1)
scalar('blc.profile[0].dft', 'BLC_OFS_G0_DFT')
put('blc.profile[0].per_channel_scale', 'true')
for channel, name in enumerate(['R', 'GR', 'GB', 'B']):
    scalar(f'blc.profile[0].channel_scale[{channel}]', 'BLC_SCALE_G0_' + name, mask=0x3ffff)
for block in ['wb', 'dg', 'bpc', 'ccm', 'gamma', 'gtm', 'nr', 'ee', 'cnf', 'aec', 'awb', 'af']:
    scalar(f'{block}.enable', 'NR_2D_CTRL' if block == 'nr' else block.upper() + '_CTRL', mask=1)
for channel in ['r', 'g', 'b']:
    scalar('wb.gain_' + channel, 'WB_GAIN_' + channel.upper())
scalar('dg.gain', 'DG_GAIN')
scalar('lsc.mesh.nx', 'LSC_MESH_NODES', mask=255)
scalar('lsc.mesh.ny', 'LSC_MESH_NODES', 8, 255)
scalar('lsc.mesh.strength', 'LSC_STRENGTH')
scalar('lsc.enable', 'LSC_CTRL', mask=1)
scalar('lsc.profile', 'LSC_PROFILE_SEL')
require(get('LSC_PROFILE_SEL') in (1, 2), 'Cold-init LSC supports resident 1 or 2 on this VP')
require(pairs_by_name.get('LSC_LOAD_CTRL', []) == [0x100 | values['LSC_PROFILE_SEL'], 0x200 | values['LSC_PROFILE_SEL']], 'LSC_LOAD_CTRL must BEGIN/VALIDATE the selected resident')
used.add('LSC_LOAD_CTRL')
count = 4 * (values['LSC_MESH_NODES'] & 255) * (values['LSC_MESH_NODES'] >> 8)
array('LSC_COEF_DATA', 'full_lsc', count)
for i in range(3):
    put(f'lsc.coefficients[{i}]', 'full_lsc' if i == values['LSC_PROFILE_SEL'] else 'NULL')
    put(f'lsc.coefficient_count[{i}]', count if i == values['LSC_PROFILE_SEL'] else 0)
require(get('BPC_MODE') == 0, 'Neon API supports spatial BPC only')
scalar('bpc.thresh_k', 'BPC_THRESH', 16, 65535)
scalar('bpc.thresh_floor', 'BPC_THRESH', mask=65535)
for i, name in enumerate(['CRR','CRG','CRB','CGR','CGG','CGB','CBR','CBG','CBB']):
    v = get('CCM_' + name) & 4095
    put(f'ccm.matrix[{i}]', v - 4096 if v & 2048 else v)
require(get('CCM_CTRL') & ~5 == 0, 'Unsupported CCM_CTRL bits')
array('GAMMA_LUT_DATA', 'full_gamma', 256)
require(get('GAMMA_LUT_ADDR') == 0, "GAMMA_LUT_ADDR must be 0: loading from a non-zero LUT address is unsupported")
require(pairs_by_name.get('GAMMA_CTRL', []) == [0x100, values['GAMMA_CTRL'] & 1], 'Gamma requires confirm then final enable/disable')
lines.append('    for (unsigned i = 0; i < 256; ++i) cfg->gamma.lut[i] = full_gamma[i];')
scalar('csc.standard', 'CSC_CTRL')
require(get('GTM_CTRL') & ~1 == 0, 'Manual GTM needs an explicit LUT schema; currently auto only')
scalar('gtm.manual', 'GTM_CTRL', 1, 1)
scalar('gtm.key', 'GTM_KEY')
scalar('gtm.white', 'GTM_LWHITE')
scalar('gtm.roi_log2', 'GTM_ROI_LOG2')
require(get('EE_CTRL') & ~1 == 0, 'Only EE resident 0 is supported by this profile schema')
scalar('ee.profile', 'EE_CTRL', 2, 3)
array('EE_LUT_WDATA', 'full_ee', 192)
require(pairs_by_name.get('EE_LUT_CTRL', []) == [0, 2, 4, 6, 0x100], "EE_LUT_CTRL must select banks 0, 1, 2, 3 in order, then confirm (0x100)")
require(pairs_by_name.get('EE_LUT_ADDR', []) == [0]*4, "EE_LUT_ADDR must contain exactly four zero addresses, one for each LUT bank")
used.update(['EE_LUT_CTRL', 'EE_LUT_ADDR'])
scalar('ee.resident[0].alpha', 'EE_ALPHA')
scalar('ee.resident[0].beta', 'EE_BETA')
scalar('ee.resident[0].clamp_positive', 'EE_CLAMP', mask=255)
scalar('ee.resident[0].clamp_negative', 'EE_CLAMP', 8, 255)
lines += ['    for (unsigned i = 0; i < 64; ++i) {',
          '        cfg->ee.resident[0].luma[i] = full_ee[i];',
          '        cfg->ee.resident[0].activity[i] = full_ee[64+i];', '    }',
          '    for (unsigned i = 0; i < 32; ++i) {',
          '        cfg->ee.resident[0].contrast_positive[i] = full_ee[128+i];',
          '        cfg->ee.resident[0].contrast_negative[i] = full_ee[160+i];', '    }']
scalar('cnf.chroma_threshold', 'CNF_CHROMA_TH')
scalar('cnf.luma_threshold', 'CNF_LUMA_TH')
require(get('CNF_CTRL') & ~5 == 0, 'Unsupported CNF_CTRL bits')
scalar('output.channel[0].resizer.enable', 'RESIZER_CTRL', mask=1)
scalar('output.channel[0].resizer.preset', 'RESIZER_CTRL', 3, 31)
scalar('aec.zones.num_zone_x', 'AEC_ZONE_CFG', mask=63)
scalar('aec.zones.num_zone_y', 'AEC_ZONE_CFG', 8, 31)
scalar('aec.zones.zone_width', 'AEC_ZONE_SIZE', mask=4095)
scalar('aec.zones.zone_height', 'AEC_ZONE_SIZE', 16, 4095)
scalar('aec.sample_min_clip', 'AEC_SAMPLE_CLIP', mask=4095)
scalar('aec.sample_max_clip', 'AEC_SAMPLE_CLIP', 16, 4095)
scalar('aec.thresh_ue', 'AEC_THRESH', mask=4095)
scalar('aec.thresh_oe', 'AEC_THRESH', 16, 4095)
scalar('awb.num_zone_x', 'AWB_CTRL', 1, 127)
scalar('awb.num_zone_y', 'AWB_CTRL', 8, 63)
scalar('awb.underexposed_limit', 'AWB_UNDEREXPOSED_LIMIT')
scalar('awb.saturation_limit', 'AWB_SATURATION_LIMIT')
for b in ['aec', 'awb', 'af']:
    scalar(b + '.context_id', b.upper() + '_CONTEXT_ID')
# Reject bits the structure mapping would otherwise silently discard.
field_masks.update(CCM_CTRL=5, CNF_CTRL=5, GAMMA_CTRL=0x101,
                   AEC_CTRL=3, RESIZER_CTRL=0xfd)
for name, mask in field_masks.items():
    for value in pairs_by_name.get(name, []):
        require(value & ~mask == 0, f'{name}: unsupported bits 0x{value & ~mask:x}')
require(used == set(values), 'Unmapped registers: ' + str(set(values) - used))
header = ('/* Generated by tools/generate_profile.py. Source SHA256: ' +
          hashlib.sha256(source.read_bytes()).hexdigest() + ' */\n'
          'static void apply_profile(isp_config_t *cfg)\n{\n' +
          '\n'.join(lines) + '\n}\n')
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(header)
print(f'Mapped {len(pairs)} writes, {len(used)} register names to Neon config')
