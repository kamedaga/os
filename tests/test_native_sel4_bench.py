"""Decoder rejects missing, duplicate, reordered-time and failed native runs."""
import unittest
from run_native_sel4_bench import parse

def fixture():
    sets=[('overhead',0,0,110),('fault',0,0,110)]
    for a in (0,1):sets += [('ipc',a,0,256),('ipc',a,4,256),('signal',a,0,110),('yield',a,0,110)]
    rows=[]
    for name,a,p,n in sets:
        for i in range(n):rows.append(f'123.456789 [Thread 61] S10S_ROW test={name} as={a} payload={p} i={i} valid=1 client_waits=1 server_waits=1 t0=100 cpu0=0 t1=200 cpu1=0 t2=300 cpu2=0 t3=400 cpu3=0')
    return '\n'.join(rows)+'\n123.456789 [Thread 61] S10S_PASS\n'
class Decode(unittest.TestCase):
    def test_complete(self):self.assertEqual(len(parse(fixture())),1684)
    def test_missing(self):
        with self.assertRaises(ValueError):parse(fixture().split('\n',1)[1])
    def test_duplicate(self):
        t=fixture()
        with self.assertRaises(ValueError):parse(t+t.splitlines()[0]+'\n')
    def test_failure(self):
        with self.assertRaises(ValueError):parse(fixture()+'S10S_FAIL site=1 rc=1\n')
    def test_missing_completion(self):
        with self.assertRaises(ValueError):parse(fixture().replace('S10S_PASS','S10S_PENDING'))
    def test_reversed_clock(self):
        with self.assertRaises(ValueError):parse(fixture().replace('t3=400','t3=99',1))
    def test_bad_endpoint(self):
        t=fixture().replace('test=fault as=0 payload=0 i=0 valid=1 client_waits=1 server_waits=1 t0=100','test=fault as=0 payload=0 i=0 valid=1 client_waits=1 server_waits=1 t0=201',1)
        with self.assertRaises(ValueError):parse(t)
if __name__=='__main__':unittest.main()
