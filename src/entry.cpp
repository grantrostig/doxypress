/*************************************************************************
 *
 * Copyright (C) 2014-2018 Barbara Geller & Ansel Sermersheim
 * Copyright (C) 1997-2014 by Dimitri van Heesch.
 * All rights reserved.
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation under the terms of the GNU General Public License version 2
 * is hereby granted. No representations are made about the suitability of
 * this software for any purpose. It is provided "as is" without express or
 * implied warranty. See the GNU General Public License for more details.
 *
 * Documents produced by DoxyPress are derivative works derived from the
 * input used in their production; they are not affected by this license.
 *
*************************************************************************/

#include <stdlib.h>

#include <entry.h>
#include <doxy_globals.h>
#include <section.h>

Entry::Entry()
{
   m_parent.clear();
   reset();
}

Entry::Entry(const Entry &e)
{
   m_tagInfoEntry   = e.m_tagInfoEntry;
   argList          = e.argList;
   typeConstr       = e.typeConstr;

   relatesType      = e.relatesType;
   virt             = e.virt;
   protection       = e.protection;
   mtype            = e.mtype;
   groupDocType     = e.groupDocType;
   m_srcLang        = e.m_srcLang;
   m_traits         = e.m_traits;

   // int
   section          = e.section;
   initLines        = e.initLines;
   docLine          = e.docLine;
   briefLine        = e.briefLine;
   inbodyLine       = e.inbodyLine;
   bodyLine         = e.bodyLine;
   endBodyLine      = e.endBodyLine;
   mGrpId           = e.mGrpId;
   startLine        = e.startLine;
   startColumn      = e.startColumn;

   // bool
   stat             = e.stat;
   explicitExternal = e.explicitExternal;
   proto            = e.proto;
   subGrouping      = e.subGrouping;
   callGraph        = e.callGraph;
   callerGraph      = e.callerGraph;
   hidden           = e.hidden;
   artificial       = e.artificial;

   // string
   m_entryName      = e.m_entryName;

   m_templateArgLists = e.m_templateArgLists;

   // list of base classes
   for (auto item : e.extends) {
      extends.append(item);
   }

   m_groups           = e.m_groups;
   m_anchors          = e.m_anchors;
   m_specialLists     = e.m_specialLists;

   // private members
   m_entryMap       = e.m_entryMap;
   m_parent         = e.m_parent;

   // deep copy, list of children
   for (auto item : e.m_sublist) {
      m_sublist.append(QMakeShared<Entry>(*item));
   }
}

Entry::~Entry()
{
}

void Entry::reset()
{
   static const bool dotCallGraph = Config::getBool("dot-call");
   static const bool dotCalledBy  = Config::getBool("dot-called-by");

   m_tagInfoEntry.clear();
   argList.clear();
   typeConstr.clear();

   relatesType  = Simple;
   virt         = Normal;
   protection   = Public;
   mtype        = Method;
   groupDocType = GROUPDOC_NORMAL;
   m_srcLang    = SrcLangExt_Unknown;

   m_traits.clear();

   // int
   section      = Entry::EMPTY_SEC;
   initLines    = -1;
   docLine      = -1;
   briefLine    = -1;
   inbodyLine   = -1;
   bodyLine     = -1;
   endBodyLine  = -1;
   mGrpId       = -1;
   startLine    = 1;
   startColumn  = 1;

   // bool
   stat             = false;
   explicitExternal = false;
   proto            = false;
   subGrouping      = true;
   callGraph        = dotCallGraph;
   callerGraph      = dotCalledBy;
   hidden           = false;
   artificial       = false;

   // string
   m_entryName      = "";

   m_templateArgLists.clear();
   extends.clear();

   m_groups.clear();
   m_anchors.clear();
   m_specialLists.clear();

   // private members
   m_entryMap.clear();
   m_sublist.clear();
}

void Entry::addSubEntry(QSharedPointer<Entry> child, QSharedPointer<Entry> dummy)
{
   QSharedPointer<Entry> self = sharedFrom(this);

   child->m_parent = self;
   m_sublist.append(child);
}

void Entry::createNavigationIndex(QSharedPointer<FileDef> fd)
{
   setFileDef(fd);

   for (auto childNode : m_sublist) {
      if (childNode->fileDef() == nullptr) {
         childNode->createNavigationIndex(fd);
      }
   }
}

void Entry::addSpecialListItem(const QString &listName, int itemId)
{
   ListItemInfo item;

   item.type   = listName;
   item.itemId = itemId;

   m_specialLists.append(item);
}

void Entry::removeSubEntry(QSharedPointer<Entry> e)
{
   // called from lex code, appears to be used when parsing Fortran only
   int i = m_sublist.indexOf(e);

   if (i != -1) {
      m_sublist.removeAt(i);
   }
}

