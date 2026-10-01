#!/usr/bin/env python3
import struct
import sys
import os

MAGIC = 0x48594452 # "HYDR"
VERSION = 1
VOCAB = 512
DIM = 64
LAYERS = 4

def generate_model_file(filename="model.hydra"):
    weights_offset = 24 # Header-Größe
    num_packed_bytes = (DIM * LAYERS)
    
    # Packe Header
    # uint32 magic, uint16 version, uint16 vocab, uint32 dim, uint32 layers, uint32 offset, uint32 len
    header = struct.pack("<IHHIIII", MAGIC, VERSION, VOCAB, DIM, LAYERS, weights_offset, num_packed_bytes)
    
    # Generiere Pseudo-Ternary-Weights: alternierend 01 (+1) und 10 (-1)
    weights = bytearray()
    for i in range(num_packed_bytes):
        # 2 Ternary values pro Byte: (w1 & 0x3) | ((w2 & 0x3) << 2)
        val = 0x09 if (i % 2 == 0) else 0x06
        weights.append(val)
        
    with open(filename, "wb") as f:
        f.write(header)
        f.write(weights)
        
    print(f"[OK] Modell '{filename}' erfolgreich gebaut ({len(header) + len(weights)} Bytes).")

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "default.hydra"
    generate_model_file(out)
