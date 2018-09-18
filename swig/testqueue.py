#!/usr/bin/env python

from __future__ import print_function
import sys, mpr, random

def h(sig, event, id, val, tt):
    print('     handler got', sig['name'], '=', val, 'at time', tt.get_double())

src = mpr.device("src")
outsig1 = src.add_signal(mpr.DIR_OUT, 1, "outsig1", 1, mpr.INT32, None, 0, 1000)
outsig2 = src.add_signal(mpr.DIR_OUT, 1, "outsig2", 1, mpr.INT32, None, 0, 1000)

dest = mpr.device("dest")
insig1 = dest.add_signal(mpr.DIR_IN, 1, "insig1", 1, mpr.FLT, None, 0, 1, h)
insig2 = dest.add_signal(mpr.DIR_IN, 1, "insig2", 1, mpr.FLT, None, 0, 1, h)

while not src.ready or not dest.ready:
    src.poll()
    dest.poll(10)

map1 = mpr.map(outsig1, insig1)
map1.push()

map2 = mpr.map(outsig2, insig2)
map2.push()

while not map1.ready or not map2.ready:
    src.poll()
    dest.poll(10)

for i in range(50):
    now = src.start_queue()
    print('Updating output signals to', i, 'at time', now.get_double())
    outsig1.set_value(i)
    outsig2.set_value(i)
    src.send_queue(now)
    dest.poll(100)
    src.poll(0)
