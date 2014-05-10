#!/usr/bin/env python

from random import random

maxsize = 4*1024*1024

for i in range(0, 100):
    print int(random()*float(maxsize))
