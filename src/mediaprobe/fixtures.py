"""Media builders shared by the tests. Safe to import: reads no arguments."""
import struct


def chunk(cid, payload):
    """RIFF chunk, padded to an even size."""
    return cid + struct.pack('<I', len(payload)) + payload + b'\0' * (len(payload) & 1)


def riff(form, *chunks):
    return chunk(b'RIFF', form + b''.join(chunks))


def wav(samples, rate, bits, format_tag=1, channels=1):
    """WAVE file with a fmt chunk and one data chunk."""
    align = channels * ((bits + 7) // 8)
    fmt = struct.pack('<HHIIHH', format_tag, channels, rate, rate * align, align, bits)
    return riff(b'WAVE', chunk(b'fmt ', fmt), chunk(b'data', samples))
