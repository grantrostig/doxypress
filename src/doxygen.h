/*************************************************************************
 *
 * Copyright (C) 1997-2014 by Dimitri van Heesch.
 * Copyright (C) 2014-2015 Barbara Geller & Ansel Sermersheim 
 * All rights reserved.    
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation under the terms of the GNU General Public License version 2
 * is hereby granted. No representations are made about the suitability of
 * this software for any purpose. It is provided "as is" without express or
 * implied warranty. See the GNU General Public License for more details.
 *
 * Documents produced by Doxygen are derivative works derived from the
 * input used in their production; they are not affected by this license.
 *
*************************************************************************/

#ifndef DOXYGEN_H
#define DOXYGEN_H

#include <QByteArray>  
#include <QFileInfo>
#include <QHash>
#include <QStringList>

#include <dirdef.h> 
#include <ftextstream.h>
#include <membergroup.h>
#include <memberlist.h>
#include <section.h>
#include <sortedlist.h>
#include <stringmap.h>

class DefinitionIntf;
class DirSDict;
class DirRelation;
class FormulaList;
class FormulaDict;
class FormulaNameDict;
class IndexList;
class RefList;
class PageSList;
class PageSDict;
class PageDef;
class SearchIndexIntf;
class ParserManager;
class ObjCache;
class Store;
class BufStr;
class CiteDict;
class MemberDef;
class GroupDef;
class GroupSDict;
class FileDef;
class ClassDef;
class ClassSDict;
class GenericsSDict;
class MemberNameSDict;
class FileNameDict;
class NamespaceSDict;
class NamespaceDef;

struct MemberGroupInfo;

class StringDict : public QHash<QString, QByteArray>
{
 public:
   StringDict() : QHash<QString, QByteArray>() 
   {}

   virtual ~StringDict()
   {}
};

int readFileOrDirectory(const QString &s, SortedList<FileName *> *fnList, FileNameDict *fnDict, StringDict *exclDict,
                        QStringList *patList, QStringList *exclPatList, QStringList *resultList, StringDict *resultDict, 
                        bool recursive, bool errorIfNotExist = true, QHash<QString, void *> *killDict = 0, QHash<QString, void *> *paths = 0);

int readDir(QFileInfo *fi, SortedList<FileName *> *fnList, FileNameDict *fnDict, StringDict *exclDict,
            QStringList *patList, QStringList *exclPatList, QStringList *resultList,
            StringDict *resultDict, bool errorIfNotExist, bool recursive, QHash<QString, void *> *killDict, 
            QHash<QString, void *> *paths);


void copyAndFilterFile(const char *fileName, BufStr &dest);

#endif
