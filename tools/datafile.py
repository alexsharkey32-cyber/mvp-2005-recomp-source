"""MVP Baseball 2005 tuning file (DATA/DATAFILE/DATAFILE.BIG -> datafile.txt): read, edit, write.

Lines come in pairs. A schema line  '0xHASH N K;idx Name;idx Name;...'  names the fields of the objects that
follow it, and each object line is  '0xHASH N<object.name>;idx value;idx value;...'.
"""
import re
class DataFile:
    def __init__(self, text):
        self.lines = text.split('\n'); self.schema_of = {}; self.index = {}
        cur, left = None, 0
        for i, l in enumerate(self.lines):
            m = re.match(r'(0x[0-9a-f]+) (\d+)<([^>]*)>;', l)
            if m:
                self.index.setdefault(m.group(3), i)
                if left > 0: self.schema_of[i] = cur; left -= 1
                else: self.schema_of[i] = {}
                continue
            m = re.match(r'(0x[0-9a-f]+) (\d+) (\d+);(.*)', l)            # schema: hash, fields, objects that follow
            if m:
                cur = {}; left = int(m.group(3))
                for part in m.group(4).split(';'):
                    if part.strip(): a, _, b = part.partition(' '); cur[b] = int(a)
    def fields(self, name):
        i = self.index[name]; sch = self.schema_of[i]; inv = {v: k for k, v in sch.items()}
        body = self.lines[i].split('>;', 1)[1]; out = {}
        for part in body.split(';'):
            if part.strip(): a, _, b = part.partition(' '); out[inv.get(int(a), a)] = b
        return out
    def set(self, name, field, value):
        i = self.index[name]; idx = self.schema_of[i][field]
        head, body = self.lines[i].split('>;', 1); parts = body.split(';'); done = False
        for k, part in enumerate(parts):
            a, _, b = part.partition(' ')
            if a.strip().isdigit() and int(a) == idx: parts[k] = '%s %s' % (a, value); done = True
        assert done, (name, field)
        self.lines[i] = head + '>;' + ';'.join(parts)
    def text(self): return '\n'.join(self.lines)
if __name__ == '__main__':
    import sys
    d = DataFile(open(sys.argv[1], encoding='latin1').read())
    if len(sys.argv) > 2:
        for n in sys.argv[2:]:
            print('==', n)
            for k, v in sorted(d.fields(n).items()): print('  %-40s %s' % (k, v[:100]))
    else:
        print(' '.join(sorted(n for n in d.index if '.' not in n)))
