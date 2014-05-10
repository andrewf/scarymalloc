#!/usr/bin/env python

from random import random

maxsize = 2*0x100

for i in range(0, 100):
    print int(random()*float(maxsize))
