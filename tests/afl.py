#! /usr/bin/python3 -B
#
# Copyright (c) 2018-2019 Gavin D. Howard and contributors.
#
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# * Redistributions of source code must retain the above copyright notice, this
#   list of conditions and the following disclaimer.
#
# * Redistributions in binary form must reproduce the above copyright notice,
#   this list of conditions and the following disclaimer in the documentation
#   and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.
#

import os
import sys
import shutil
import subprocess

def usage():
	print("usage: {} dir [exe results_dir]".format(script))
	sys.exit(1)

def check_crash(exebase, out, error, file, type, test):
	if error < 0:
		print("\n{} crashed ({}) on {}:\n".format(exebase, -error, type))
		print("    {}".format(test))
		print("\nCopying to \"{}\"".format(out))
		shutil.copy2(file, out)
		print("\nexiting...")
		sys.exit(error)

def run_test(exe, exebase, tout, indata, out, file, type, test):
	try:
		p = subprocess.run(exe, timeout=tout, input=indata, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
		check_crash(exebase, out, p.returncode, file, type, test)
	except subprocess.TimeoutExpired:
		print("\n    {} timed out. Continuing...\n".format(exebase))

def get_children(dir, get_files):
	dirs = []
	with os.scandir(dir) as it:
		for entry in it:
			if not entry.name.startswith('.') and     \
			   ((entry.is_dir() and not get_files) or \
			    (entry.is_file() and get_files)):
				dirs.append(entry.name)
	dirs.sort()
	return dirs

script = sys.argv[0]
testdir = os.path.dirname(script)

if __name__ != "__main__":
	usage()

tout = 3

if len(sys.argv) < 2:
	usage()

exedir = sys.argv[1]

if len(sys.argv) >= 3:
	exe = sys.argv[2]
else:
	exe = testdir + "/../bin/" + exedir

exebase = os.path.basename(exe)

if exebase == "bc":
	halt = "halt\n"
	options = "-lq"
else:
	halt = "q\n"
	options = "-x"

if len(sys.argv) >= 4:
	resultsdir = sys.argv[3]
else:
	if exedir == "bc":
		resultsdir = testdir + "/../../results"
	else:
		resultsdir = testdir + "/../../results_dc"

exe = [ exe, options ]
for i in range(4, len(sys.argv)):
	exe.append(sys.argv[i])

out = testdir + "/../.test.txt"

print(os.path.realpath(os.getcwd()))

dirs = get_children(resultsdir, False)

for d in dirs:

	d = resultsdir + "/" + d

	print(d)

	files = get_children(d + "/crashes/", True)

	for file in files:

		file = d + "/crashes/" + file

		print("    {}".format(file))

		base = os.path.basename(file)

		if base == "README.txt":
			continue

		with open(file, "rb") as f:
			lines = f.readlines()

		for l in lines:
			run_test(exe, exebase, tout, l, out, file, "test", l)

		print("        Running whole file...")

		run_test(exe + [ file ], exebase, tout, halt.encode(), out, file, "file", file)

		print("        Running file through stdin...")

		with open(file, "rb") as f:
			content = f.read()

		run_test(exe, exebase, tout, content, out, file, "running {} through stdin".format(file), file)

print("Done")

