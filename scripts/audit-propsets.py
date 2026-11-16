#!/usr/bin/env python3
"""Audit HostSupport (and Support) against the OFX property-set metadata.

NatronGitHub/openfx#79.

The reference is the metadata in include/ (``@propdef``, ``@propset``,
``@propsetdef`` and ``@actiondef`` blocks, see scripts/ofx_prop_utils.py).

What HostSupport declares is taken from a runtime dump made by
HostSupport/examples/propsetDump (it writes the property sets HostSupport really
creates, as JSON). A runtime dump is used instead of parsing the PropSpec arrays,
because which arrays are added, and under which #ifdef, depends on the parameter
type, the action and the build flags; the dump shows exactly what a plug-in sees
for a given flag set. Build propsetDump once per flag set and pass each dump.

For each property set (host descriptor, effect descriptor/instance, clip
descriptor/instance, image, interact descriptor/instance, every parameter type,
every action's inArgs/outArgs) it reports:
  - missing: listed by the metadata, not declared by HostSupport
    (split into required and host_optional);
  - extra: declared by HostSupport, not listed (extension properties are marked);
  - type and dimension mismatches;
  - with --check-read-only, plug-in read-only flag mismatches. HostSupport currently
    drops the flag (PropertyTemplate ignores pluginReadOnly), so every property is
    plug-in writable; see NatronGitHub/openfx#76.
Known, deliberate differences are listed in KNOWN below and reported separately.

It also lists the properties the Support library reads or writes that HostSupport
never declares in any dumped set (they would fail on a HostSupport host).

Usage:
  scripts/audit-propsets.py [-i include] [--support Support] [--json out.json]
                            [--strict] dump1.json [dump2.json ...]
--strict exits with status 1 if there are unexplained required-missing, type,
dimension or read-only findings.
"""

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ofx_prop_utils as u  # noqa: E402

# metadata property-set name for each param type dumped by propsetDump
# (None: no upstream property set; compared with the common param lists only)
PARAM_SETS = {
    'OfxParamTypeInteger': 'ParamsByte',
    'OfxParamTypeDouble': 'ParamsDouble1D',
    'OfxParamTypeBoolean': None,
    'OfxParamTypeChoice': 'ParamsChoice',
    'OfxParamTypeStrChoice': 'ParamsStrChoice',
    'OfxParamTypeRGBA': 'ParamsRGBA',
    'OfxParamTypeRGB': 'ParamsRGB',
    'OfxParamTypeDouble2D': 'ParamsDouble2D3D',
    'OfxParamTypeDouble3D': 'ParamsDouble2D3D',
    'OfxParamTypeInteger2D': 'ParamsInt2D3D',
    'OfxParamTypeInteger3D': 'ParamsInt2D3D',
    'OfxParamTypeString': 'ParamsString',
    'OfxParamTypeCustom': 'ParamsCustom',
    'OfxParamTypeBytes': None,
    'OfxParamTypeGroup': 'ParamsGroup',
    'OfxParamTypePage': 'ParamsPage',
    'OfxParamTypePushButton': None,
    'OfxParamTypeParametric': 'ParamsParametric',
}
# fallback lists for param types without an upstream set
PARAM_FALLBACK = {
    'OfxParamTypeBoolean': ['ParamsCommon_REF', 'ParamsAllButGroupPage_REF', 'ParamsValue_REF'],
    'OfxParamTypeBytes': ['ParamsCommon_REF', 'ParamsAllButGroupPage_REF', 'ParamsValue_REF'],
    'OfxParamTypePushButton': ['ParamsCommon_REF', 'ParamsAllButGroupPage_REF'],
}

# Deliberate or understood differences: (set pattern, property) -> reason.
# The set pattern is a regular expression matched against the dumped set name.
KNOWN = [
    (r'Param(Descriptor|Instance):.*', 'OfxParamPropHasHostOverlayHandle|kOfxParamPropUseHostOverlayHandle',
     'host overlay handles: declared by hosts that draw them (Natron adds them in its '
     'OfxImageEffectDescriptor::paramDefine override; note that the value of kOfxParamPropUseHostOverlayHandle '
     'really is "kOfxParamPropUseHostOverlayHandle")'),
    (r'Param(Descriptor|Instance):OfxParamTypeString',
     'OfxParamPropMin|OfxParamPropMax|OfxParamPropDisplayMin|OfxParamPropDisplayMax',
     'upstream metadata artifact: ParamsString includes ParamsNumeric_REF, but the spec text '
     'defines these for "a numeric parameter" only'),
    (r'InteractInstance', 'OfxInteractPropHasAlpha|OfxInteractPropBitDepth',
     'reachable: the interact instance chains its descriptor set (Interact::Instance constructor)'),
    (r'InteractInstance', 'OfxInteractPropSuggestedColour',
     'dimension: the spec says "double X 3 if set, else double X 0"; a get hook supplies the value'),
    (r'action:.*', 'OfxImageEffectPropBehaviourWhenUnlicensed',
     'absent on purpose: the spec uses its absence to mean "no licensing", so declaring it '
     'would claim support HostSupport does not have'),
    (r'Param(Descriptor|Instance):.*', 'OfxParamPropColourManagement',
     'absent on purpose: a failed set tells the plug-in the host has no param colour management'),
    (r'action:OfxImageEffectAction(Render|BeginSequenceRender|EndSequenceRender):inArgs',
     'OfxImageEffectPropCudaRenderSupported|OfxImageEffectPropCudaStreamSupported|'
     'OfxImageEffectPropMetalRenderSupported|OfxImageEffectPropOpenCLRenderSupported|'
     'OfxImageEffectPropOpenCLSupported|OfxImageEffectPropOpenCLImage',
     'upstream metadata artifact: descriptor capabilities and an image property listed in the '
     'render inArgs by the generated property sets; not render arguments in the spec text'),
    (r'action:.*', 'OfxImageEffectPropNoSpatialAwareness',
     'tracked in NatronGitHub/openfx#78'),
    (r'EffectDescriptor', 'OfxImageEffectPropNoSpatialAwareness',
     'tracked in NatronGitHub/openfx#78'),
    (r'action:.*', 'OfxImageEffectPropOpenGLEnabled|OfxImageEffectPropOpenGLTextureIndex|'
     'OfxImageEffectPropOpenGLTextureTarget',
     'only declared with OFX_SUPPORTS_OPENGLRENDER, i.e. when the host can do OpenGL rendering'),
]


def norm_type(t):
    """metadata type -> set of HostSupport types it may be stored as"""
    if isinstance(t, list):
        r = set()
        for x in t:
            r |= norm_type(x)
        return r
    t = str(t)
    return {
        'int': {'int'}, 'bool': {'int'}, 'double': {'double'},
        'string': {'string'}, 'enum': {'string'}, 'pointer': {'pointer'},
    }.get(t, {t})


def norm_dim(d):
    if d in (None, 'N', 'n', 0, '0'):
        return 0
    return int(d)


def expand(entries, sets):
    """expand _REF entries and strip annotations; returns {prop: {annotations}}"""
    out = {}
    for e in entries or []:
        e = str(e)
        if e.endswith('_REF'):
            out.update(expand(sets.get(e[:-4] + '_DEF', []), sets))
            continue
        name, _, ann = e.partition('|')
        a = {}
        for kv in ann.split(','):
            kv = kv.strip()
            if '=' in kv:
                k, v = kv.split('=', 1)
                a[k.strip()] = v.strip()
        out[name.strip()] = a
    return out


def expected_for(setname, sets, actions):
    """(props, write) expected for a dumped set name, or (None, None)"""
    if setname.startswith('action:'):
        _, rest = setname.split(':', 1)
        action, _, rest2 = rest.partition(':')
        which = rest2.split(':')[-1]  # inArgs / outArgs (instanceChanged has an extra :type)
        if action not in actions:
            return None, None
        return expand(actions[action].get(which) or [], sets), ('host' if which == 'inArgs' else 'plugin')
    if setname.startswith('ParamDescriptor:') or setname.startswith('ParamInstance:'):
        ptype = setname.split(':', 1)[1]
        ps = PARAM_SETS.get(ptype)
        if ps:
            return expand(sets[ps]['props'], sets), sets[ps].get('write', 'plugin')
        if ptype in PARAM_FALLBACK:
            return expand(PARAM_FALLBACK[ptype], sets), 'plugin'
        return None, None
    if setname in sets and isinstance(sets[setname], dict):
        return expand(sets[setname]['props'], sets), sets[setname].get('write')
    return None, None


def known_reason(setname, prop):
    for pat, props, why in KNOWN:
        if re.fullmatch(pat, setname) and prop in props.split('|'):
            return why
    return None


def support_props(support_dir):
    """property names (string values) the Support library passes to propGet*/propSet*"""
    names = set()
    for f in Path(support_dir).rglob('*.cpp'):
        for m in re.finditer(r'prop(?:Get|Set)\w*\(\s*(k\w+)', f.read_text(errors='replace')):
            names.add(m.group(1))
    return names


def define_values(include_dir):
    vals = {}
    p = Path(include_dir)
    for h in ([p] if p.is_file() else list(p.rglob('*.h'))):
        for m in re.finditer(r'#define\s+(k\w+)\s+"([^"]+)"', h.read_text(errors='replace')):
            vals.setdefault(m.group(1), m.group(2))
    return vals


def audit(dump, props, sets, actions, ext_props, check_ro=False):
    findings = []  # (category, set, prop, detail)
    for setname, declared in dump['sets'].items():
        exp, write = expected_for(setname, sets, actions)
        if exp is None:
            findings.append(('unmapped-set', setname, '', 'no metadata property set to compare with'))
            continue
        for prop, ann in exp.items():
            if prop not in declared:
                why = known_reason(setname, prop)
                if why:
                    findings.append(('known', setname, prop, why))
                elif ann.get('host_optional') == 'true':
                    findings.append(('missing-optional', setname, prop, ''))
                else:
                    findings.append(('missing', setname, prop, ''))
                continue
            d = declared[prop]
            meta = props.get(prop)
            if meta:
                if d['type'] not in norm_type(meta.get('type')):
                    findings.append(('type', setname, prop, f"HostSupport {d['type']}, spec {meta.get('type')}"))
                md = norm_dim(meta.get('dimension'))
                # a variable dimension in the spec means "depends on the object" (e.g. a
                # param's default has the param's dimension): any HostSupport size is fine
                if md != 0 and md != d['dimension'] and not known_reason(setname, prop):
                    findings.append(('dimension', setname, prop,
                                     f"HostSupport {d['dimension'] or 'variable'}, spec {md or 'variable'}"))
            pw = ann.get('write', write)
            exp_ro = (pw == 'host')
            # descriptors (written by the plug-in) and outArgs: plug-in may write;
            # host-written sets and inArgs: read-only for the plug-in
            if check_ro and exp_ro != d['pluginReadOnly'] and not setname.startswith('Param'):
                findings.append(('read-only', setname, prop,
                                 f"HostSupport pluginReadOnly={d['pluginReadOnly']}, spec write={pw}"))
        for prop in declared:
            if prop not in exp:
                kind = 'extra-extension' if (prop in ext_props or prop not in props) else 'extra'
                findings.append((kind, setname, prop, ''))
    return findings


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dumps', nargs='+', help='JSON files written by HostSupport/examples/propsetDump')
    ap.add_argument('-i', '--include', default='include')
    ap.add_argument('--support', default='Support')
    ap.add_argument('--json', help='write all findings as JSON')
    ap.add_argument('--strict', action='store_true')
    ap.add_argument('--check-read-only', action='store_true',
                    help='also compare the plug-in read-only flags (meaningless until HostSupport '
                         'stores them: PropertyTemplate ignores pluginReadOnly, see #76)')
    args = ap.parse_args()

    props = u.get_properties_from_headers(args.include)
    sets = u.get_propsets_from_headers(args.include)
    actions = u.get_actions_from_headers(args.include)
    # extension properties: @propdef blocks with an "extension:" key (also nuke/, tuttle/)
    # and every property defined in an extension header (most of ofxNatron.h has no "extension:" key)
    ext_headers = ('ofxNatron.h', 'ofxSonyVegas.h', 'ofxOld.h')
    ext_props = set()
    for h in Path(args.include).rglob('*.h'):
        in_ext_header = h.name in ext_headers or h.parent.name in ('nuke', 'tuttle')
        for name, meta in u.extract_inline_metadata(h).items():
            if in_ext_header or (isinstance(meta, dict) and meta.get('extension')):
                ext_props.add(name)
        if in_ext_header:
            ext_props |= set(define_values(h).values())
    values = define_values(args.include)

    all_out = {}
    unexplained = 0
    for path in args.dumps:
        dump = json.loads(Path(path).read_text())
        f = audit(dump, props, sets, actions, ext_props, args.check_read_only)
        label = f"{Path(path).name} [{', '.join(dump.get('defines', [])) or 'no extensions'}]"
        counts = {}
        for c, *_ in f:
            counts[c] = counts.get(c, 0) + 1
        print(f"\n=== {label}: {len(dump['sets'])} property sets")
        print('    ' + ', '.join(f'{k}: {v}' for k, v in sorted(counts.items())))
        for cat in ('missing', 'type', 'dimension', 'read-only', 'missing-optional', 'known',
                    'extra', 'extra-extension', 'unmapped-set'):
            rows = [x for x in f if x[0] == cat]
            if not rows:
                continue
            print(f"\n  [{cat}]")
            for _, s, p, d in rows:
                print(f"    {s:55s} {p:50s} {d}")
        unexplained += sum(1 for x in f if x[0] in ('missing', 'type', 'dimension', 'read-only'))
        all_out[label] = [dict(category=c, set=s, prop=p, detail=d) for c, s, p, d in f]

        # Support: properties it uses that this HostSupport build never declares
        declared_any = set()
        for s in dump['sets'].values():
            declared_any |= set(s)
        used = {values.get(k, k) for k in support_props(args.support)}
        not_declared = sorted(p for p in used if p not in declared_any and p in props)
        print(f"\n  [support-uses-undeclared] {len(not_declared)} standard properties used by Support "
              f"but not declared in any dumped set (may be fine: sets not dumped, or optional):")
        for p in not_declared:
            print(f"    {p}")

    if args.json:
        Path(args.json).write_text(json.dumps(all_out, indent=1))
    if args.strict and unexplained:
        print(f"\naudit-propsets: {unexplained} unexplained finding(s)")
        return 1
    print("\naudit-propsets: done")
    return 0


if __name__ == '__main__':
    sys.exit(main())
