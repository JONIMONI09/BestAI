#!/usr/bin/env python3
"""Recognise a GGUF file and report whether it can be converted.

    python3 tools/gguf_inspect.py model.gguf
    python3 tools/gguf_inspect.py model.gguf --json

Read-only: nothing is written, nothing is loaded into an engine. The point
is to answer "what did I just download, and will Hydra take it?" before
spending minutes on a conversion that ends in a refusal.
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gguf_reader  # noqa: E402

#: Printed in full instead of "…N more", because the architecture and the
#: context length are the two fields people actually look for.
SHOW_KEYS = ('general.architecture', 'general.name', 'general.file_type',
             'general.quantization_version', 'general.alignment',
             'general.context_length', 'general.embedding_length',
             'general.block_count', 'general.parameter_count',
             'tokenizer.model', 'tokenizer.ggml.model')


def inspect(path):
    data = open(path, 'rb').read()
    report = {'path': os.path.basename(path), 'bytes': len(data)}

    # Detection before parsing: the console and the Android app both need
    # "what is this file" to work on a file that is not a GGUF at all.
    if len(data) < 24:
        report.update(ok=False, format=None,
                      error='file is only %d bytes - too short to identify' % len(data))
        return report
    magic = int.from_bytes(data[:4], 'little')
    report['magic'] = '0x%08X' % magic
    if magic != gguf_reader.GGUF_MAGIC:
        report.update(ok=False, format=None,
                      error='not a GGUF file (magic 0x%08X, expected 0x%08X "GGUF")'
                            % (magic, gguf_reader.GGUF_MAGIC))
        return report
    report['format'] = 'GGUF'

    try:
        gguf = gguf_reader.GgufFile(data, path)
    except gguf_reader.GgufError as exc:
        report.update(ok=False, error=str(exc))
        return report

    dense = gguf.dense_candidates()
    decodable = gguf.decodable_candidates()
    quantised = gguf.quantised_names()
    convertible = bool(decodable) and min(t.shape()[1] for t in decodable) > 0

    report['ok'] = True
    report['version'] = gguf.version
    report['architecture'] = gguf.architecture()
    report['alignment'] = gguf.alignment
    report['tensorCount'] = len(gguf.tensors)
    report['tokens'] = len(gguf.tokens())
    report['denseFloatTensors'] = len(dense)
    report['blockDequantisedTensors'] = len(decodable) - len(dense)
    report['quantisedTensors'] = quantised
    report['convertible'] = convertible
    report['metadata'] = {
        k: v for k, v in gguf.metadata.items()
        if k in SHOW_KEYS or k.startswith('general.')
    }
    report['tensors'] = [t.as_dict() for t in gguf.tensors]
    if not convertible:
        report['convertibleWhy'] = (
            'every 1-D/2-D tensor uses a block layout this reader does not '
            'implement (%s). Hydra decodes F32, F16, BF16 and Q8_0: '
            're-export with llama.cpp --convert-f16.'
            % ', '.join(quantised[:6] if quantised else ['no readable tensor']))
    return report


def main(argv=None):
    ap = argparse.ArgumentParser(
        description='Report what a GGUF file contains and whether Hydra can convert it.')
    ap.add_argument('path')
    ap.add_argument('--json', action='store_true', help='machine-readable output')
    ap.add_argument('--tensors', action='store_true', help='list every tensor')
    args = ap.parse_args(argv)

    try:
        report = inspect(args.path)
    except OSError as exc:
        report = {'path': os.path.basename(args.path), 'ok': False, 'error': str(exc)}

    if args.json:
        json.dump(report, sys.stdout, ensure_ascii=False)
        sys.stdout.write('\n')
        return 0 if report.get('ok') else 1

    if not report.get('ok'):
        print('%s: not convertible - %s' % (report['path'], report.get('error')))
        return 1

    print('%s  GGUF v%d  %d bytes' % (report['path'], report['version'],
                                      report['bytes']))
    print('  architecture : %s' % (report['architecture'] or 'unknown'))
    print('  tensors      : %d total, %d dense float, %d Q8_0 dequantised, '
          '%d unsupported' % (report['tensorCount'],
                              report['denseFloatTensors'],
                              report['blockDequantisedTensors'],
                              len(report['quantisedTensors'])))
    print('  vocabulary   : %d tokens' % report['tokens'])
    for key, value in report['metadata'].items():
        if isinstance(value, list):
            value = '[%d entries]' % len(value)
        print('  %-28s %s' % (key, value))
    if report['quantisedTensors']:
        print('  not read     : %s' % ', '.join(report['quantisedTensors'][:8]))
    print('  convertible  : %s' % ('yes' if report['convertible'] else
                                   'NO - ' + report.get('convertibleWhy', '')))
    if args.tensors:
        for t in report['tensors']:
            print('    %-44s %-10s %s' % (t['name'], t['type'], t['dims']))
    return 0


if __name__ == '__main__':
    sys.exit(main())