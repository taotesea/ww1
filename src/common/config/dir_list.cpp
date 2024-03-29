/*
 *	PROGRAM:	Client/Server Common Code
 *	MODULE:		dir_list.cpp
 *	DESCRIPTION:	Directory listing config file operation
 *
 *  The contents of this file are subject to the Initial
 *  Developer's Public License Version 1.0 (the "License");
 *  you may not use this file except in compliance with the
 *  License. You may obtain a copy of the License at
 *  http://www.ibphoenix.com/main.nfs?a=ibphoenix&page=ibp_idpl.
 *
 *  Software distributed under the License is distributed AS IS,
 *  WITHOUT WARRANTY OF ANY KIND, either express or implied.
 *  See the License for the specific language governing rights
 *  and limitations under the License.
 *
 *  The Original Code was created by Alexander Peshkoff
 *  for the Firebird Open Source RDBMS project.
 *
 *  Copyright (c) 2003 Alexander Peshkoff <peshkoff@mail.ru>
 *  and all contributors signed below.
 *
 *  All Rights Reserved.
 *  Contributor(s): ______________________________________.
 */

#include "firebird.h"
#include "../common/config/config.h"
#include "../common/config/dir_list.h"
#include "../jrd/os/path_utils.h"
#include "../jrd/gds_proto.h"

#define New FB_NEW(*getDefaultMemoryPool())

ParsedPath::ParsedPath(void) {
	PathElem = 0;
	nElem = 0;
}

ParsedPath::ParsedPath(const Firebird::PathName& path) {
	PathElem = 0;
	Parse(path);
}

ParsedPath::~ParsedPath() {
	delete[] PathElem;
}

void ParsedPath::Parse(const Firebird::PathName& path) {
	delete[] PathElem;

	if (path.length() == 1) {
		nElem = 1;
		PathElem = New Firebird::PathName[1];
		PathElem[0] = path;
		return;
	}

	nElem = 0;
	Firebird::PathName oldpath = path;
	do {
		Firebird::PathName newpath, elem;
		PathUtils::splitLastComponent(newpath, elem, oldpath);
		oldpath = newpath;
		nElem++;
	} while (oldpath.length() > 0);

	PathElem = New Firebird::PathName[nElem];
	oldpath = path;
	for (int i = nElem; i--; ) {
		Firebird::PathName newpath;
		PathUtils::splitLastComponent(newpath, PathElem[i], oldpath);
		oldpath = newpath;
	}
}

bool ParsedPath::operator==(const char* path) const {
	return (Firebird::PathName(*this) == Firebird::PathName(path));
}

Firebird::PathName ParsedPath::SubPath(int n) const {
	Firebird::PathName rc = PathElem[0];
	if (PathUtils::isRelative(rc + PathUtils::dir_sep))
		rc = PathUtils::dir_sep + rc;
	for (int i = 1; i < n; i++) {
		Firebird::PathName newpath;
		PathUtils::concatPath(newpath, rc, PathElem[i]);
		rc = newpath;
	}
	return rc;
}

ParsedPath::operator Firebird::PathName() const {
	if (!PathElem)
		return "";
	return SubPath(nElem);
}

bool ParsedPath::Contains(const ParsedPath& pPath) const {
	int nFullElem = nElem;
	if (nFullElem > 1 && PathElem[nFullElem - 1].length() == 0)
		nFullElem--;

	if (pPath.nElem < nFullElem) {
		return false;
	}
	int i;
	for (i = 0; i < nFullElem; i++) {
		if (!PathUtils::comparePaths(pPath.PathElem[i], PathElem[i])) {
			return false;
		}
	}
	for (i = nFullElem + 1; i <= pPath.nElem; i++) {
		Firebird::PathName x = pPath.SubPath(i);
		if (PathUtils::isSymLink(x)) {
			return false;
		}
	}
	return true;
}

DirectoryList::DirectoryList() {
	ConfigDirs = 0;
	nDirs = 0;
	Mode = NotInitialized;
}

DirectoryList::~DirectoryList() {
	Clear();
}

bool DirectoryList::KeyWord(
		const ListMode KeyMode, 
		Firebird::PathName& Value, 
		Firebird::PathName Key, 
		Firebird::PathName Next
) {
	if (Value.length() < Key.length()) {
		return false;
	}
	Firebird::PathName KeyValue = Value.substr(0, Key.length());
	if (KeyValue != Key) {
		return false;
	}
	if (Next.length() > 0) {
		if (Value.length() == Key.length()) {
			return false;
		}
		KeyValue = Value.substr(Key.length());
		if (Next.find(KeyValue[0]) == Firebird::PathName::npos) {
			return false;
		}
		Firebird::PathName::size_type startPos = 
			KeyValue.find_first_not_of(Next);
		if (startPos == Firebird::PathName::npos) {
			return false;
		}
		Value = KeyValue.substr(startPos);
	}
	else {
		if (Value.length() > Key.length()) {
			return false;
		}
		Value.erase();
	}
	Mode = KeyMode;
	return true;
}

void DirectoryList::Initialize(bool simple_mode) {
	if (Mode != NotInitialized)
		return;

	Clear();

	Firebird::PathName val = GetConfigString();

	if (simple_mode) {
		Mode = SimpleList;
	}
	else {
		if (KeyWord(None, val, "None", "") || 
			KeyWord(Full, val, "Full", "")) {
			return;
		}
		if (! KeyWord(Restrict, val, "Restrict", " \t")) {
			gds__log("DirectoryList: unknown parameter '%s', "
				"defaulting to None", val.c_str());
			Mode = None;
			return;
		}
	}

	nDirs = 1;
	unsigned int i;
	for (i = 0; i < val.length(); i++) {
		if (val[i] == ';') {
			nDirs++;
		}
	}
	ConfigDirs = New ParsedPath[nDirs];
	unsigned int Last = 0;
	nDirs = 0;
	Firebird::PathName Root = Config::getRootDirectory();
	for (i = 0; i < val.length(); i++) {
		if (val[i] == ';') {
			Firebird::PathName dir = "";
			if (i > Last) {
				dir = val.substr(Last, i-Last);
				Trim(dir);
			}
			if (PathUtils::isRelative(dir)) {
				Firebird::PathName newdir;
				PathUtils::concatPath(newdir, Root, dir);
				dir = newdir;
			}
			ConfigDirs[nDirs++].Parse(dir);
			Last = i + 1;
		}
	}
	Firebird::PathName dir = "";
	if (i > Last) {
		dir = val.substr(Last, i - Last);
		Trim(dir);
	}
	if (PathUtils::isRelative(dir)) {
		Firebird::PathName newdir;
		PathUtils::concatPath(newdir, Root, dir);
		dir = newdir;
	}
	ConfigDirs[nDirs++].Parse(dir);
}

bool DirectoryList::IsPathInList(const Firebird::PathName& path) {
#ifdef BOOT_BUILD
	return true;
#else  //BOOT_BUILD
	Initialize();

	// Handle special cases
	switch(Mode) {
	case NotInitialized:
	case None:
		return false;
	case Full:
		return true;
	}

	// Disable any up-dir(..) references - in case our path_utils
	// and OS handle paths in slightly different ways,
	// this is "wonderful" potential hole for hacks
	// Example of IIS attack attempt:
	// "GET /scripts/..%252f../winnt/system32/cmd.exe?/c+dir HTTP/1.0"
	//								(live from apache access.log :)
	if (path.find(PathUtils::up_dir_link) != Firebird::PathName::npos)
		return false;

	Firebird::PathName varpath = path;
	if (PathUtils::isRelative(path)) {
		PathUtils::concatPath(varpath, 
			Firebird::PathName(Config::getRootDirectory()), path);
	}

	ParsedPath pPath = path;
    bool rc = 0;
    for (int i = 0; i < nDirs; i++) {
		if (ConfigDirs[i].Contains(pPath)) {
			rc = true;
			break;
		}
	}
	return rc;
#endif //BOOT_BUILD
}

void DirectoryList::ExpandFileName (
					Firebird::PathName & Path, 
					const Firebird::PathName & Name,
					int Access
) {
	if (Mode == NotInitialized)
		Initialize();
    for (int i = 0; i < nDirs; i++) {
		PathUtils::concatPath(Path, ConfigDirs[i], Name);
		if (PathUtils::canAccess(Path, Access)) {
			return;
		}
	}
	Path = Name;
}

TempDirectoryList::TempDirectoryList() : items(0)
{
	Initialize(true);

	// Get directory list
	const ParsedPath* dir_list = DirList();

	// Iterate through directories to parse them
	// and fill the "items" vector
	for (int i = 0; i < DirCount(); i++) {
		Item item;
		Firebird::PathName dir = dir_list[i];
		int pos = dir.rfind(" ");
		long size = atol(dir.substr(pos + 1, Firebird::PathName::npos).c_str());
		if (pos != Firebird::PathName::npos && !size) {
			pos = Firebird::PathName::npos;
		}
		if (size <= 0) {
			size = ALLROOM;
		}
		item.dir = dir.substr(0, pos);
		item.size = size;
		items.push_back(item);
	}
}

const Firebird::PathName TempDirectoryList::GetConfigString() const
{
	const char* value;
	char tmp_buf[MAXPATHLEN];
	if (!(value = Config::getTempDirectories())) {
		// Temporary directory configuration has not been defined.
		// Let's make default configuration.
		gds__temp_dir(tmp_buf);
		value = tmp_buf;
	}
	return Firebird::PathName(value);
}

size_t TempDirectoryList::Count() const
{
	return items.size();
}

const TempDirectoryList::Item& TempDirectoryList::operator[](size_t i) const
{
	return items[i];
}
