with open("src/opcodes.h", "rt", encoding='utf-8') as f:
	OPCODES_TEXT = f.read()

pipeline = [
	["hash_table8.hpp", "__stl__.h", "memory.h", "str.h", "safestl.h", "builtins.h", "error.h"],
	["obj.h", "iter.h", "parser.h", "pointer.h", "codeobject.h"],
	["vm.h", "compiler.h", "repl.h"],
	["pocketpy.h"]
]

copied = set()

text = ""

import re
import shutil
import os
import time

if os.path.exists("amalgamated"):
	shutil.rmtree("amalgamated")
	time.sleep(1)
os.mkdir("amalgamated")

def remove_copied_include(text):
	text = text.replace("#pragma once", "")
	text = re.sub(
		r'#include\s+"(.+)"\s*',
		lambda m: "" if m.group(1) in copied else m.group(0),
		text
	)
	text = text.replace('#include "opcodes.h"', OPCODES_TEXT)
	return text

for seq in pipeline:
	for j in seq:
		with open("src/"+j, "rt", encoding='utf-8') as f:
			text += remove_copied_include(f.read()) + '\n'
			copied.add(j)

with open("amalgamated/pocketpy.h", "wt", encoding='utf-8') as f:
	final_text = \
r'''/*
 *  Copyright (c) 2022 blueloveTH
 *  Distributed Under The LGPLv3 License
 */

#ifndef POCKETPY_H
#define POCKETPY_H
''' + text + '\n#endif // POCKETPY_H'
	f.write(final_text)

shutil.copy("src/main.cpp", "amalgamated/main.cpp")
os.system("g++ -o pocketpy amalgamated/main.cpp --std=c++17 -pthread")
os.system("rm pocketpy")

os.system("cp amalgamated/pocketpy.h plugins/flutter/src/pocketpy.h")
os.system('cp amalgamated/pocketpy.h "plugins/unity/My project/Assets/com.bl.pocketpy/Plugins/iOS/pocketpy.h"')

if os.path.exists("plugins/godot/godot-cpp/pocketpy"):
	os.system("cp amalgamated/pocketpy.h plugins/godot/godot-cpp/pocketpy/src/pocketpy.h")