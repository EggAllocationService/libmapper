#!/usr/bin/env python

from __future__ import print_function
import sys, random, libmapper as mpr

print('starting testvector.py')
print('libmapper version:', mpr.__version__, 'with' if mpr.has_numpy() else 'without', 'numpy support')

def h(sig, event, id, val, time):
    print('  handler got', sig['name'], '=', val, 'at time', time.get_double())

def h2(sig, event, id, val, time):
    print('  handler2 got', sig['name'], '=', val, 'at time', time.get_double())

src = mpr.Device("py.testvector.src")
outsig = src.add_signal(mpr.Direction.OUTGOING, "outsig", 10, mpr.Type.INT32, None, 0, 1)

dest = mpr.Device("py.testvector.dst")
insig = dest.add_signal(mpr.Direction.INCOMING, "insig", 10, mpr.Type.FLOAT, None, 0, 1, None, h)

insig.set_callback(h2)

while not src.ready or not dest.ready:
    src.poll(10)
    dest.poll(10)

map = mpr.Map(outsig, insig)
map.push()

while not map.ready:
    src.poll(10)
    dest.poll(10)

for i in range(100):
    outsig.set_value([i, i+1, i+2, i+3, i+4, i+5, i+6, i+7, i+8, i+9])
    dest.poll(10)
    src.poll(0)

print('freeing devices')
src.free()
dest.free()
print('done')
