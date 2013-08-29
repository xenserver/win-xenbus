#!/usr/bin/env python

import os, sys

file = os.popen('git status -u --porcelain')

for line in file:
    item = line.split(' ')
    if item[0] == '??':
        path = ' '.join(item[1:]).rstrip()
        print(path)
        os.remove(path)

file.close()
