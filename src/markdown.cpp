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

#include <qglobal.h>
#include <QRegExp>
#include <QFileInfo>
#include <QHash>

#include <stdio.h>

#include <config.h>
#include <markdown.h>
#include <growbuf.h>
#include <doxygen.h>
#include <commentscan.h>
#include <entry.h>
#include <bufstr.h>
#include <commentcnv.h>
#include <section.h>
#include <message.h>
#include <util.h>

// must appear after the previous include - resolve soon 
#include <doxy_globals.h>

// is character at position i in data part of an identifier?
#define isIdChar(i) \
  ((data[i]>='a' && data[i]<='z') || \
   (data[i]>='A' && data[i]<='Z') || \
   (data[i]>='0' && data[i]<='9'))   \
 
// is character at position i in data allowed before an emphasis section
#define isOpenEmphChar(i) \
  (data[i]=='\n' || data[i]==' ' || data[i]=='\'' || data[i]=='<' || \
   data[i]=='{'  || data[i]=='(' || data[i]=='['  || data[i]==',' || \
   data[i]==':'  || data[i]==';')

// is character at position i in data an escape that prevents ending an emphasis section
// so for example *bla (*.txt) is cool*
#define ignoreCloseEmphChar(i) \
  (data[i]=='('  || data[i]=='{' || data[i]=='[' || data[i]=='<' || \
   data[i]=='='  || data[i]=='+' || data[i]=='-' || data[i]=='\\' || \
   data[i]=='@')

struct LinkRef {
   LinkRef(const QByteArray &l, const QByteArray &t) : link(l), title(t) {}
   QByteArray link;
   QByteArray title;
};

typedef int (*action_t)(GrowBuf &out, const char *data, int offset, int size);

enum Alignment { AlignNone, AlignLeft, AlignCenter, AlignRight };

static QHash<QString, LinkRef> g_linkRefs;

static action_t       g_actions[256];
static Entry         *g_current;
static QByteArray     g_fileName;
static int            g_lineNr;

// In case a markdown page starts with a level1 header, that header is used
// as a title of the page, in effect making it a level0 header, so the
// level of all other sections needs to be corrected as well.
// This flag is true if corrections are needed.
//static bool           g_correctSectionLevel;

const int codeBlockIndent = 4;

static void processInline(GrowBuf &out, const char *data, int size);


// escape characters that have a special meaning later on.
static QByteArray escapeSpecialChars(const QByteArray &s)
{
   if (s.isEmpty()) {
      return "";
   }

   GrowBuf growBuf;
   const char *p = s;
   char c;
   while ((c = *p++)) {
      switch (c) {
         case '<':
            growBuf.addStr("\\<");
            break;
         case '>':
            growBuf.addStr("\\>");
            break;
         case '\\':
            growBuf.addStr("\\\\");
            break;
         case '@':
            growBuf.addStr("\\@");
            break;
         default:
            growBuf.addChar(c);
            break;
      }
   }
   growBuf.addChar(0);
   return growBuf.get();
}

static void convertStringFragment(QByteArray &result, const char *data, int size)
{
   if (size < 0) {
      size = 0;
   }

   result.resize(size + 1);
   memcpy(result.data(), data, size);
  
   result[size] = '\0';
}

/** helper function to convert presence of left and/or right alignment markers
 *  to a alignment value
 */
static Alignment markersToAlignment(bool leftMarker, bool rightMarker)
{
   //printf("markerToAlignment(%d,%d)\n",leftMarker,rightMarker);
   if (leftMarker && rightMarker) {
      return AlignCenter;
   } else if (leftMarker) {
      return AlignLeft;
   } else if (rightMarker) {
      return AlignRight;
   } else {
      return AlignNone;
   }
}


// Check if data contains a block command. If so returned the command
// that ends the block. If not an empty string is returned.
// Note When offset>0 character position -1 will be inspected.
//
// Checks for and skip the following block commands:
// {@code .. { .. } .. }
// \dot .. \enddot
// \code .. \endcode
// \msc .. \endmsc
// \f$..\f$
// \f[..\f]
// \f{..\f}
// \verbatim..\endverbatim
// \latexonly..\endlatexonly
// \htmlonly..\endhtmlonly
// \xmlonly..\endxmlonly
// \rtfonly..\endrtfonly
// \manonly..\endmanonly
static QByteArray isBlockCommand(const char *data, int offset, int size)
{
   bool openBracket = offset > 0 && data[-1] == '{';
   bool isEscaped = offset > 0 && (data[-1] == '\\' || data[-1] == '@');
   if (isEscaped) {
      return QByteArray();
   }

   int end = 1;
   while (end < size && (data[end] >= 'a' && data[end] <= 'z')) {
      end++;
   }
   if (end == 1) {
      return QByteArray();
   }

   QByteArray blockName;
   convertStringFragment(blockName, data + 1, end - 1);

   if (blockName == "code" && openBracket) {
      return "}";

   } else if (blockName == "dot"         ||
              blockName == "code"        ||
              blockName == "msc"         ||
              blockName == "verbatim"    ||
              blockName == "latexonly"   ||
              blockName == "htmlonly"    ||
              blockName == "xmlonly"     ||
              blockName == "rtfonly"     ||
              blockName == "manonly"     ||
              blockName == "docbookonly" ) {
      return "end" + blockName;

   } else if (blockName == "startuml") {
      return "enduml";

   } else if (blockName == "f" && end < size) {
      if (data[end] == '$') {
         return "f$";

      } else if (data[end] == '[') {
         return "f]";

      } else if (data[end] == '}') {
         return "f}";

      }
   }
   return QByteArray();
}

/** looks for the next emph char, skipping other constructs, and
 *  stopping when either it is found, or we are at the end of a paragraph.
 */
static int findEmphasisChar(const char *data, int size, char c, int c_size)
{
   int i = 1;

   while (i < size) {
      while (i < size && data[i] != c    && data[i] != '`' &&
             data[i] != '\\' && data[i] != '@' &&
             data[i] != '\n') {
         i++;
      }
      //printf("findEmphasisChar: data=[%s] i=%d c=%c\n",data,i,data[i]);

      // not counting escaped chars or characters that are unlikely
      // to appear as the end of the emphasis char
      if (i > 0 && ignoreCloseEmphChar(i - 1)) {
         i++;
         continue;
      } else {
         // get length of emphasis token
         int len = 0;
         while (i + len < size && data[i + len] == c) {
            len++;
         }

         if (len > 0) {
            if (len != c_size || (i < size - len && isIdChar(i + len))) { // to prevent touching some_underscore_identifier
               i = i + len;
               continue;
            }
            return i; // found it
         }
      }

      // skipping a code span
      if (data[i] == '`') {
         int snb = 0;
         while (i < size && data[i] == '`') {
            snb++, i++;
         }

         // find same pattern to end the span
         int enb = 0;
         while (i < size && enb < snb) {
            if (data[i] == '`') {
               enb++;
            }
            if (snb == 1 && data[i] == '\'') {
               break;   // ` ended by '
            }
            i++;
         }
      } else if (data[i] == '@' || data[i] == '\\') {
         // skip over blocks that should not be processed
         QByteArray endBlockName = isBlockCommand(data + i, i, size - i);
         if (!endBlockName.isEmpty()) {
            i++;
            int l = endBlockName.length();
            while (i < size - l) {
               if ((data[i] == '\\' || data[i] == '@') && // command
                     data[i - 1] != '\\' && data[i - 1] != '@') { // not escaped
                  if (qstrncmp(&data[i + 1], endBlockName, l) == 0) {
                     break;
                  }
               }
               i++;
            }
         } else if (i < size - 1 && isIdChar(i + 1)) { // @cmd, stop processing, see bug 690385
            return 0;
         } else {
            i++;
         }
      } else if (data[i] == '\n') { // end * or _ at paragraph boundary
         i++;
         while (i < size && data[i] == ' ') {
            i++;
         }
         if (i >= size || data[i] == '\n') {
            return 0;   // empty line -> paragraph
         }
      } else { // should not get here!
         i++;
      }

   }
   return 0;
}

/** process single emphasis */
static int processEmphasis1(GrowBuf &out, const char *data, int size, char c)
{
   int i = 0, len;

   /* skipping one symbol if coming from emph3 */
   if (size > 1 && data[0] == c && data[1] == c) {
      i = 1;
   }

   while (i < size) {
      len = findEmphasisChar(data + i, size - i, c, 1);
      if (len == 0) {
         return 0;
      }
      i += len;
      if (i >= size) {
         return 0;
      }

      if (i + 1 < size && data[i + 1] == c) {
         i++;
         continue;
      }
      if (data[i] == c && data[i - 1] != ' ' && data[i - 1] != '\n') {
         out.addStr("<em>");
         processInline(out, data, i);
         out.addStr("</em>");
         return i + 1;
      }
   }
   return 0;
}

/** process double emphasis */
static int processEmphasis2(GrowBuf &out, const char *data, int size, char c)
{
   int i = 0, len;

   while (i < size) {
      len = findEmphasisChar(data + i, size - i, c, 2);
      if (len == 0) {
         return 0;
      }
      i += len;
      if (i + 1 < size && data[i] == c && data[i + 1] == c && i && data[i - 1] != ' ' &&
            data[i - 1] != '\n'
         ) {
         out.addStr("<strong>");
         processInline(out, data, i);
         out.addStr("</strong>");
         return i + 2;
      }
      i++;
   }
   return 0;
}

/** Parsing tripple emphasis.
 *  Finds the first closing tag, and delegates to the other emph
 */
static int processEmphasis3(GrowBuf &out, const char *data, int size, char c)
{
   int i = 0, len;

   while (i < size) {
      len = findEmphasisChar(data + i, size - i, c, 3);
      if (len == 0) {
         return 0;
      }
      i += len;

      /* skip whitespace preceded symbols */
      if (data[i] != c || data[i - 1] == ' ' || data[i - 1] == '\n') {
         continue;
      }

      if (i + 2 < size && data[i + 1] == c && data[i + 2] == c) {
         out.addStr("<em><strong>");
         processInline(out, data, i);
         out.addStr("</strong></em>");
         return i + 3;
      } else if (i + 1 < size && data[i + 1] == c) {
         // double symbol found, handing over to emph1
         len = processEmphasis1(out, data - 2, size + 2, c);
         if (len == 0) {
            return 0;
         } else {
            return len - 2;
         }
      } else {
         // single symbol found, handing over to emph2
         len = processEmphasis2(out, data - 1, size + 1, c);
         if (len == 0) {
            return 0;
         } else {
            return len - 1;
         }
      }
   }
   return 0;
}

/** Process ndash and mdashes */
static int processNmdash(GrowBuf &out, const char *data, int off, int size)
{
   // precondition: data[0]=='-'
   int i = 1;
   int count = 1;
   if (i < size && data[i] == '-') { // found --
      count++, i++;
   }
   if (i < size && data[i] == '-') { // found ---
      count++, i++;
   }
   if (i < size && data[i] == '-') { // found ----
      count++;
   }
   if (count == 2 && (off < 8 || qstrncmp(data - 8, "operator", 8) != 0)) { // -- => ndash
      out.addStr("&ndash;");
      return 2;
   } else if (count == 3) { // --- => ndash
      out.addStr("&mdash;");
      return 3;
   }
   // not an ndash or mdash
   return 0;
}

/** Process quoted section "...", can contain one embedded newline */
static int processQuoted(GrowBuf &out, const char *data, int, int size)
{
   int i = 1;
   int nl = 0;
   while (i < size && data[i] != '"' && nl < 2) {
      if (data[i] == '\n') {
         nl++;
      }
      i++;
   }
   if (i < size && data[i] == '"' && nl < 2) {
      out.addStr(data, i + 1);
      return i + 1;
   }
   // not a quoted section
   return 0;
}

/** Process a HTML tag. Note that <pre>..</pre> are treated specially, in
 *  the sense that all code inside is written unprocessed
 */
static int processHtmlTag(GrowBuf &out, const char *data, int offset, int size)
{
   if (offset > 0 && data[-1] == '\\') {
      return 0;   // escaped <
   }

   // find the end of the html tag
   int i = 1;
   int l = 0;

   // compute length of the tag name
   while (i < size && isIdChar(i)) {
      i++, l++;
   }

   QByteArray tagName;
   convertStringFragment(tagName, data + 1, i - 1);

   if (tagName.toLower() == "pre") { 
      // found <pre> tag
      bool insideStr = false;

      while (i < size - 6) {
         char c = data[i];

         if (! insideStr && c == '<') {
            // potential start of html tag

            if (data[i + 1] == '/' && tolower(data[i + 2]) == 'p' && tolower(data[i + 3]) == 'r' &&
                  tolower(data[i + 4]) == 'e' && tolower(data[i + 5]) == '>') {

               // found </pre> tag, copy from start to end of tag
               out.addStr(data, i + 6);
              
               return i + 6;
            }

         } else if (insideStr && c == '"') {
            if (data[i - 1] != '\\') {
               insideStr = false;
            }
         } else if (c == '"') {
            insideStr = true;
         }
         i++;
      }
   } else { // some other html tag
      if (l > 0 && i < size) {
         if (data[i] == '/' && i < size - 1 && data[i + 1] == '>') { // <bla/>
            //printf("Found htmlTag={%s}\n",QByteArray(data).left(i+2).data());
            out.addStr(data, i + 2);
            return i + 2;

         } else if (data[i] == '>') { // <bla>
            //printf("Found htmlTag={%s}\n",QByteArray(data).left(i+1).data());
            out.addStr(data, i + 1);
            return i + 1;

         } else if (data[i] == ' ') { // <bla attr=...
            i++;
            bool insideAttr = false;
            while (i < size) {
               if (!insideAttr && data[i] == '"') {
                  insideAttr = true;
               } else if (data[i] == '"' && data[i - 1] != '\\') {
                  insideAttr = false;
               } else if (!insideAttr && data[i] == '>') { // found end of tag
                  //printf("Found htmlTag={%s}\n",QByteArray(data).left(i+1).data());
                  out.addStr(data, i + 1);
                  return i + 1;
               }
               i++;
            }
         }
      }
   }
   //printf("Not a valid html tag\n");
   return 0;
}

static int processEmphasis(GrowBuf &out, const char *data, int offset, int size)
{
   if ((offset > 0 && !isOpenEmphChar(-1)) || // invalid char before * or _
         (size > 1 && data[0] != data[1] && !isIdChar(1)) || // invalid char after * or _
         (size > 2 && data[0] == data[1] && !isIdChar(2))) { // invalid char after ** or __
      return 0;
   }

   char c = data[0];
   int ret;
   if (size > 2 && data[1] != c) { // _bla or *bla
      // whitespace cannot follow an opening emphasis
      if (data[1] == ' ' || data[1] == '\n' ||
            (ret = processEmphasis1(out, data + 1, size - 1, c)) == 0) {
         return 0;
      }
      return ret + 1;
   }
   if (size > 3 && data[1] == c && data[2] != c) { // __bla or **bla
      if (data[2] == ' ' || data[2] == '\n' ||
            (ret = processEmphasis2(out, data + 2, size - 2, c)) == 0) {
         return 0;
      }
      return ret + 2;
   }
   if (size > 4 && data[1] == c && data[2] == c && data[3] != c) { // ___bla or ***bla
      if (data[3] == ' ' || data[3] == '\n' ||
            (ret = processEmphasis3(out, data + 3, size - 3, c)) == 0) {
         return 0;
      }
      return ret + 3;
   }
   return 0;
}

static int processLink(GrowBuf &out, const char *data, int, int size)
{
   QByteArray content;
   QByteArray link;
   QByteArray title;

   int contentStart, contentEnd, linkStart, titleStart, titleEnd;
   bool isImageLink = false;
   bool isToc = false;
   int i = 1;

   if (data[0] == '!') {
      isImageLink = true;
      if (size < 2 || data[1] != '[') {
         return 0;
      }
      i++;
   }

   contentStart = i;
   int level = 1;
   int nl = 0;

   // find the matching ]
   while (i < size) {
      if (data[i - 1] == '\\') { // skip escaped characters
      } else if (data[i] == '[') {
         level++;
      } else if (data[i] == ']') {
         level--;
         if (level <= 0) {
            break;
         }
      } else if (data[i] == '\n') {
         nl++;
         if (nl > 1) {
            return 0;   // only allow one newline in the content
         }
      }
      i++;
   }
   if (i >= size) {
      return 0;   // premature end of comment -> no link
   }
   contentEnd = i;
   convertStringFragment(content, data + contentStart, contentEnd - contentStart);
   //printf("processLink: content={%s}\n",content.data());
   if (!isImageLink && content.isEmpty()) {
      return 0;   // no link text
   }
   i++; // skip over ]

   // skip whitespace
   while (i < size && data[i] == ' ') {
      i++;
   }
   if (i < size && data[i] == '\n') { // one newline allowed here
      i++;
      // skip more whitespace
      while (i < size && data[i] == ' ') {
         i++;
      }
   }

   bool explicitTitle = false;
   if (i < size && data[i] == '(') { // inline link
      i++;
      while (i < size && data[i] == ' ') {
         i++;
      }
      if (i < size && data[i] == '<') {
         i++;
      }
      linkStart = i;
      nl = 0;
      while (i < size && data[i] != '\'' && data[i] != '"' && data[i] != ')') {
         if (data[i] == '\n') {
            nl++;
            if (nl > 1) {
               return 0;
            }
         }
         i++;
      }
      if (i >= size || data[i] == '\n') {
         return 0;
      }
      convertStringFragment(link, data + linkStart, i - linkStart);
      link = link.trimmed();

      //printf("processLink: link={%s}\n",link.data());
      if (link.isEmpty()) {
         return 0;
      }

      if (link.at(link.length() - 1) == '>') {
         link = link.left(link.length() - 1);
      }

      // optional title
      if (data[i] == '\'' || data[i] == '"') {
         char c = data[i];
         i++;
         titleStart = i;
         nl = 0;

         while (i < size && data[i] != ')') {
            if (data[i] == '\n') {
               if (nl > 1) {
                  return 0;
               }
               nl++;
            }
            i++;
         }

            if (i >= size) {
            return 0;
         }

         titleEnd = i - 1;
         // search back for closing marker
         while (titleEnd > titleStart && data[titleEnd] == ' ') {
            titleEnd--;
         }
         if (data[titleEnd] == c) { // found it
            convertStringFragment(title, data + titleStart, titleEnd - titleStart);
            //printf("processLink: title={%s}\n",title.data());
         } else {
            return 0;
         }
      }
      i++;

   } else if (i < size && data[i] == '[') { 
      // reference link
      i++;
      linkStart = i;
      nl = 0;

      // find matching ]
      while (i < size && data[i] != ']') {
         if (data[i] == '\n') {
            nl++;
            if (nl > 1) {
               return 0;
            }
         }
         i++;
      }

      if (i >= size) {
         return 0;
      }

      // extract link
      convertStringFragment(link, data + linkStart, i - linkStart);
     
      link = link.trimmed();

      if (link.isEmpty()) { 
         // shortcut link
         link = content;
      }

      // lookup reference
      auto lr = g_linkRefs.find(link.toLower());

      if (lr != g_linkRefs.end()) { 
         // found it
         link  = lr->link;
         title = lr->title;      

      } else { 
         // reference not found        
         return 0;
      }
      i++;

   } else if (i < size && data[i] != ':' && !content.isEmpty()) { 
      // minimal link ref notation [some id]

      auto lr = g_linkRefs.find(content.toLower());

      if (lr != g_linkRefs.end()) { 
         // found it
         link  = lr->link;
         title = lr->title;
         explicitTitle = true;
         i = contentEnd;

      } else if (content == "TOC") {
         isToc = true;
         i = contentEnd;

      } else {
         return 0;
      }
      i++;
   } else {
      return 0;
   }

   static QRegExp re("^[@\\]ref ");
   if (isToc) { 
      // special case for [TOC]
      if (g_current) {
         g_current->stat = true;
      }

   } else if (isImageLink) {
      bool ambig;
      FileDef *fd = 0;

      if (link.indexOf("@ref ") != -1 || link.indexOf("\\ref ") != -1 || (fd = findFileDef(Doxygen::imageNameDict, link, ambig)))
         // assume doxygen symbol link or local image link

      {
         out.addStr("@image html ");
         out.addStr(link.mid(fd ? 0 : 5));

         if (!explicitTitle && !content.isEmpty()) {
            out.addStr(" \"");
            out.addStr(content);
            out.addStr("\"");

         } else if ((content.isEmpty() || explicitTitle) && !title.isEmpty()) {
            out.addStr(" \"");
            out.addStr(title);
            out.addStr("\"");
         }
      } else {
         out.addStr("<img src=\"");
         out.addStr(link);
         out.addStr("\" alt=\"");
         out.addStr(content);
         out.addStr("\"");

         if (!title.isEmpty()) {
            out.addStr(" title=\"");
            out.addStr(substitute(title.simplified(), "\"", "&quot;"));
            out.addStr("\"");
         }
         out.addStr("/>");
      }

   } else {
      SrcLangExt lang = getLanguageFromFileName(link);
      int lp = -1;

      if ((lp = link.indexOf("@ref ")) != -1 || (lp = link.indexOf("\\ref ")) != -1 || lang == SrcLangExt_Markdown)
         // assume doxygen symbol link
      {
         if (lp == -1) { // link to markdown page
            out.addStr("@ref ");
         }
         out.addStr(link);
         out.addStr(" \"");
         if (explicitTitle && !title.isEmpty()) {
            out.addStr(title);
         } else {
            out.addStr(content);
         }
         out.addStr("\"");

      } else if (link.indexOf('/') != -1 || link.indexOf('.') != -1 || link.indexOf('#') != -1) {
         // file/url link
         out.addStr("<a href=\"");
         out.addStr(link);
         out.addStr("\"");

         if (!title.isEmpty()) {
            out.addStr(" title=\"");
            out.addStr(substitute(title.simplified(), "\"", "&quot;"));
            out.addStr("\"");
         }

         out.addStr(">");
         out.addStr(content.simplified());
         out.addStr("</a>");

      } else { 
         // avoid link to e.g. F[x](y)
         //printf("no link for '%s'\n",link.data());
         return 0;
      }
   }
   return i;
}

/** '`' parsing a code span (assuming codespan != 0) */
static int processCodeSpan(GrowBuf &out, const char *data, int /*offset*/, int size)
{
   int end, nb = 0, i, f_begin, f_end;

   /* counting the number of backticks in the delimiter */
   while (nb < size && data[nb] == '`') {
      nb++;
   }

   /* finding the next delimiter */
   i = 0;
   int nl = 0;
   for (end = nb; end < size && i < nb && nl < 2; end++) {
      if (data[end] == '`') {
         i++;
      } else if (data[end] == '\n') {
         i = 0;
         nl++;
      } else {
         i = 0;
      }
   }
   if (i < nb && end >= size) {
      return 0;  // no matching delimiter
   }
   if (nl == 2) { // too many newlines inside the span
      return 0;
   }

   // trimming outside whitespaces
   f_begin = nb;
   while (f_begin < end && data[f_begin] == ' ') {
      f_begin++;
   }
   f_end = end - nb;
   while (f_end > nb && data[f_end - 1] == ' ') {
      f_end--;
   }

   if (nb == 1) { // check for closing ' followed by space within f_begin..f_end
      i = f_begin;
      while (i < f_end - 1) {
         if (data[i] == '\'' && !isIdChar(i + 1)) { // reject `some word' and not `it's cool`
            return 0;
         }
         i++;
      }
   }
   //printf("found code span '%s'\n",QByteArray(data+f_begin).left(f_end-f_begin).data());

   /* real code span */
   if (f_begin < f_end) {
      QByteArray codeFragment;
      convertStringFragment(codeFragment, data + f_begin, f_end - f_begin);
      out.addStr("<tt>");
      //out.addStr(convertToHtml(codeFragment,true));
      out.addStr(escapeSpecialChars(codeFragment));
      out.addStr("</tt>");
   }
   return end;
}


static int processSpecialCommand(GrowBuf &out, const char *data, int offset, int size)
{
   int i = 1;
   QByteArray endBlockName = isBlockCommand(data, offset, size);
   if (!endBlockName.isEmpty()) {
      int l = endBlockName.length();
      while (i < size - l) {
         if ((data[i] == '\\' || data[i] == '@') && // command
               data[i - 1] != '\\' && data[i - 1] != '@') { // not escaped
            if (qstrncmp(&data[i + 1], endBlockName, l) == 0) {
               //printf("found end at %d\n",i);
               out.addStr(data, i + 1 + l);
               return i + 1 + l;
            }
         }
         i++;
      }
   }
   if (size > 1 && data[0] == '\\') {
      char c = data[1];
      if (c == '[' || c == ']' || c == '*' || c == '+' || c == '-' ||
            c == '!' || c == '(' || c == ')' || c == '.' || c == '`' || c == '_') {
         if (c == '-' && size > 3 && data[2] == '-' && data[3] == '-') { // \---
            out.addStr(&data[1], 3);
            return 4;
         } else if (c == '-' && size > 2 && data[2] == '-') { // \--
            out.addStr(&data[1], 2);
            return 3;
         }
         out.addStr(&data[1], 1);
         return 2;
      }
   }
   return 0;
}

static void processInline(GrowBuf &out, const char *data, int size)
{
   int i = 0, end = 0;
   action_t action = 0;
   while (i < size) {
      while (end < size && ((action = g_actions[(uchar)data[end]]) == 0)) {
         end++;
      }
      out.addStr(data + i, end - i);
      if (end >= size) {
         break;
      }
      i = end;
      end = action(out, data + i, i, size - i);
      if (!end) {
         end = i + 1;
      } else {
         i += end;
         end = i;
      }
   }
}

/** returns whether the line is a setext-style hdr underline */
static int isHeaderline(const char *data, int size)
{
   int i = 0, c = 0;
   while (i < size && data[i] == ' ') {
      i++;
   }

   // test of level 1 header
   if (data[i] == '=') {
      while (i < size && data[i] == '=') {
         i++, c++;
      }
      while (i < size && data[i] == ' ') {
         i++;
      }
      return (c > 1 && (i >= size || data[i] == '\n')) ? 1 : 0;
   }
   // test of level 2 header
   if (data[i] == '-') {
      while (i < size && data[i] == '-') {
         i++, c++;
      }
      while (i < size && data[i] == ' ') {
         i++;
      }
      return (c > 1 && (i >= size || data[i] == '\n')) ? 2 : 0;
   }
   return 0;
}

/** returns true if this line starts a block quote */
static bool isBlockQuote(const char *data, int size, int indent)
{
   int i = 0;
   while (i < size && data[i] == ' ') {
      i++;
   }
   if (i < indent + codeBlockIndent) { // could be a quotation
      // count >'s and skip spaces
      int level = 0;
      while (i < size && (data[i] == '>' || data[i] == ' ')) {
         if (data[i] == '>') {
            level++;
         }
         i++;
      }
      // last characters should be a space or newline,
      // so a line starting with >= does not match
      return level > 0 && i < size && ((data[i - 1] == ' ') || data[i] == '\n');
   } else { // too much indentation -> code block
      return false;
   }
   //return i<size && data[i]=='>' && i<indent+codeBlockIndent;
}

/** returns end of the link ref if this is indeed a link reference. */
static int isLinkRef(const char *data, int size,
                     QByteArray &refid, QByteArray &link, QByteArray &title)
{
   //printf("isLinkRef data={%s}\n",data);
   // format: start with [some text]:
   int i = 0;
   while (i < size && data[i] == ' ') {
      i++;
   }
   if (i >= size || data[i] != '[') {
      return 0;
   }
   i++;
   int refIdStart = i;
   while (i < size && data[i] != '\n' && data[i] != ']') {
      i++;
   }
   if (i >= size || data[i] != ']') {
      return 0;
   }
   convertStringFragment(refid, data + refIdStart, i - refIdStart);
   if (refid.isEmpty()) {
      return 0;
   }
   //printf("  isLinkRef: found refid='%s'\n",refid.data());
   i++;
   if (i >= size || data[i] != ':') {
      return 0;
   }
   i++;

   // format: whitespace* \n? whitespace* (<url> | url)
   while (i < size && data[i] == ' ') {
      i++;
   }
   if (i < size && data[i] == '\n') {
      i++;
      while (i < size && data[i] == ' ') {
         i++;
      }
   }
   if (i >= size) {
      return 0;
   }

   if (i < size && data[i] == '<') {
      i++;
   }
   int linkStart = i;
   while (i < size && data[i] != ' ' && data[i] != '\n') {
      i++;
   }
   int linkEnd = i;
   if (i < size && data[i] == '>') {
      i++;
   }
   if (linkStart == linkEnd) {
      return 0;   // empty link
   }
   convertStringFragment(link, data + linkStart, linkEnd - linkStart);
   //printf("  isLinkRef: found link='%s'\n",link.data());
   if (link == "@ref" || link == "\\ref") {
      int argStart = i;
      while (i < size && data[i] != '\n' && data[i] != '"') {
         i++;
      }
      QByteArray refArg;
      convertStringFragment(refArg, data + argStart, i - argStart);
      link += refArg;
   }

   title.resize(0);

   // format: (whitespace* \n? whitespace* ( 'title' | "title" | (title) ))?
   int eol = 0;
   while (i < size && data[i] == ' ') {
      i++;
   }
   if (i < size && data[i] == '\n') {
      eol = i;
      i++;
      while (i < size && data[i] == ' ') {
         i++;
      }
   }
   if (i >= size) {
      //printf("end of isLinkRef while looking for title! i=%d\n",i);
      return i; // end of buffer while looking for the optional title
   }

   char c = data[i];
   if (c == '\'' || c == '"' || c == '(') { // optional title present?
      //printf("  start of title found! char='%c'\n",c);
      i++;
      if (c == '(') {
         c = ')';   // replace c by end character
      }
      int titleStart = i;
      // search for end of the line
      while (i < size && data[i] != '\n') {
         i++;
      }
      eol = i;

      // search back to matching character
      int end = i - 1;
      while (end > titleStart && data[end] != c) {
         end--;
      }
      if (end > titleStart) {
         convertStringFragment(title, data + titleStart, end - titleStart);
      }
      //printf("  title found: '%s'\n",title.data());
   }
   while (i < size && data[i] == ' ') {
      i++;
   }
   //printf("end of isLinkRef: i=%d size=%d data[i]='%c' eol=%d\n",
   //    i,size,data[i],eol);
   if      (i >= size) {
      return i;   // end of buffer while ref id was found
   } else if (eol) {
      return eol;   // end of line while ref id was found
   }
   return 0;                            // invalid link ref
}

static int isHRuler(const char *data, int size)
{
   int i = 0;
   if (size > 0 && data[size - 1] == '\n') {
      size--;   // ignore newline character
   }
   while (i < size && data[i] == ' ') {
      i++;
   }
   if (i >= size) {
      return 0;   // empty line
   }
   char c = data[i];
   if (c != '*' && c != '-' && c != '_') {
      return 0; // not a hrule character
   }
   int n = 0;
   while (i < size) {
      if (data[i] == c) {
         n++; // count rule character
      } else if (data[i] != ' ') {
         return 0; // line contains non hruler characters
      }
      i++;
   }
   return n >= 3; // at least 3 characters needed for a hruler
}

static QByteArray extractTitleId(QByteArray &title)
{
   //static QRegExp r1("^[a-z_A-Z][a-z_A-Z0-9\\-]*:");
   static QRegExp r2("\\{#[a-z_A-Z][a-z_A-Z0-9\\-]*\\}");

   int l = 0;
   int i = r2.indexIn(title);
   l = r2.matchedLength();

   if (i != -1 && title.mid(i + l).trimmed().isEmpty()) { 
      // found {#id} style id
      QByteArray id = title.mid(i + 2, l - 3);
      title = title.left(i);

      //printf("found id='%s' title='%s'\n",id.data(),title.data());
      return id;
   }

   //printf("no id found in title '%s'\n",title.data());
   return "";
}


static int isAtxHeader(const char *data, int size, QByteArray &header, QByteArray &id)
{
   int i = 0, end;
   int level = 0, blanks = 0;

   // find start of header text and determine heading level
   while (i < size && data[i] == ' ') {
      i++;
   }
   if (i >= size || data[i] != '#') {
      return 0;
   }
   while (i < size && level < 6 && data[i] == '#') {
      i++, level++;
   }
   while (i < size && data[i] == ' ') {
      i++, blanks++;
   }
   if (level == 1 && blanks == 0) {
      return 0; // special case to prevent #someid seen as a header (see bug 671395)
   }

   // find end of header text
   end = i;
   while (end < size && data[end] != '\n') {
      end++;
   }
   while (end > i && (data[end - 1] == '#' || data[end - 1] == ' ')) {
      end--;
   }

   // store result
   convertStringFragment(header, data + i, end - i);
   id = extractTitleId(header);
   if (!id.isEmpty()) { // strip #'s between title and id
      i = header.length() - 1;
      while (i >= 0 && (header.at(i) == '#' || header.at(i) == ' ')) {
         i--;
      }
      header = header.left(i + 1);
   }

   return level;
}

static int isEmptyLine(const char *data, int size)
{
   int i = 0;
   while (i < size) {
      if (data[i] == '\n') {
         return true;
      }
      if (data[i] != ' ') {
         return false;
      }
      i++;
   }
   return true;
}

#define isLiTag(i) \
   (data[(i)]=='<' && \
   (data[(i)+1]=='l' || data[(i)+1]=='L') && \
   (data[(i)+2]=='i' || data[(i)+2]=='I') && \
   (data[(i)+3]=='>'))

// compute the indent from the start of the input, excluding list markers
// such as -, -#, *, +, 1., and <li>
static int computeIndentExcludingListMarkers(const char *data, int size)
{
   int i = 0;
   int indent = 0;
   bool isDigit = false;
   bool isLi = false;
   bool listMarkerSkipped = false;
   while (i < size &&
          (data[i] == ' ' ||                                  // space
           (!listMarkerSkipped &&                             // first list marker
            (data[i] == '+' || data[i] == '-' || data[i] == '*' || // unordered list char
             (data[i] == '#' && i > 0 && data[i - 1] == '-') || // -# item
             (isDigit = (data[i] >= '1' && data[i] <= '9')) || // ordered list marker?
             (isLi = (i < size - 3 && isLiTag(i)))            // <li> tag
            )
           )
          )
         ) {
      if (isDigit) { // skip over ordered list marker '10. '
         int j = i + 1;
         while (j < size && ((data[j] >= '0' && data[j] <= '9') || data[j] == '.')) {
            if (data[j] == '.') { // should be end of the list marker
               if (j < size - 1 && data[j + 1] == ' ') { // valid list marker
                  listMarkerSkipped = true;
                  indent += j + 1 - i;
                  i = j + 1;
                  break;
               } else { // not a list marker
                  break;
               }
            }
            j++;
         }
      } else if (isLi) {
         i += 3; // skip over <li>
         indent += 3;
         listMarkerSkipped = true;
      } else if (data[i] == '-' && i < size - 2 && data[i + 1] == '#' && data[i + 2] == ' ') {
         // case "-# "
         listMarkerSkipped = true; // only a single list marker is accepted
         i++; // skip over #
         indent++;
      } else if (data[i] != ' ' && i < size - 1 && data[i + 1] == ' ') {
         // case "- " or "+ " or "* "
         listMarkerSkipped = true; // only a single list marker is accepted
      }
      if (data[i] != ' ' && !listMarkerSkipped) {
         // end of indent
         break;
      }
      indent++, i++;
   }
   //printf("{%s}->%d\n",QByteArray(data).left(size).data(),indent);
   return indent;
}

static bool isFencedCodeBlock(const char *data, int size, int refIndent,
                              QByteArray &lang, int &start, int &end, int &offset)
{
   // rules: at least 3 ~~~, end of the block same amount of ~~~'s, otherwise
   // return false
   int i = 0;
   int indent = 0;
   int startTildes = 0;
   while (i < size && data[i] == ' ') {
      indent++, i++;
   }
   if (indent >= refIndent + 4) {
      return false;   // part of code block
   }
   while (i < size && data[i] == '~') {
      startTildes++, i++;
   }
   if (startTildes < 3) {
      return false;   // not enough tildes
   }
   if (i < size && data[i] == '{') {
      i++;   // skip over optional {
   }
   int startLang = i;
   while (i < size && (data[i] != '\n' && data[i] != '}' && data[i] != ' ')) {
      i++;
   }
   convertStringFragment(lang, data + startLang, i - startLang);
   while (i < size && data[i] != '\n') {
      i++;   // proceed to the end of the line
   }
   start = i;
   while (i < size) {
      if (data[i] == '~') {
         end = i - 1;
         int endTildes = 0;
         while (i < size && data[i] == '~') {
            endTildes++, i++;
         }
         while (i < size && data[i] == ' ') {
            i++;
         }
         if (i == size || data[i] == '\n') {
            offset = i;
            return endTildes == startTildes;
         }
      }
      i++;
   }
   return false;
}

static bool isCodeBlock(const char *data, int offset, int size, int &indent)
{
   //printf("<isCodeBlock(offset=%d,size=%d,indent=%d)\n",offset,size,indent);
   // determine the indent of this line
   int i = 0;
   int indent0 = 0;
   while (i < size && data[i] == ' ') {
      indent0++, i++;
   }

   if (indent0 < codeBlockIndent) {
      //printf(">isCodeBlock: line is not indented enough %d<4\n",indent0);
      return false;
   }
   if (indent0 >= size || data[indent0] == '\n') { // empty line does not start a code block
      //printf("only spaces at the end of a comment block\n");
      return false;
   }

   i = offset;
   int nl = 0;
   int nl_pos[3];
   // search back 3 lines and remember the start of lines -1 and -2
   while (i > 0 && nl < 3) {
      if (data[i - offset - 1] == '\n') {
         nl_pos[nl++] = i - offset;
      }
      i--;
   }

   // if there are only 2 preceding lines, then line -2 starts at -offset
   if (i == 0 && nl == 2) {
      nl_pos[nl++] = -offset;
   }
   //printf("  nl=%d\n",nl);

   if (nl == 3) { // we have at least 2 preceding lines
      //printf("  positions: nl_pos=[%d,%d,%d] line[-2]='%s' line[-1]='%s'\n",
      //    nl_pos[0],nl_pos[1],nl_pos[2],
      //    QByteArray(data+nl_pos[1]).left(nl_pos[0]-nl_pos[1]-1).data(),
      //    QByteArray(data+nl_pos[2]).left(nl_pos[1]-nl_pos[2]-1).data());

      // check that line -1 is empty
      if (!isEmptyLine(data + nl_pos[1], nl_pos[0] - nl_pos[1] - 1)) {
         return false;
      }

      // determine the indent of line -2
      indent = computeIndentExcludingListMarkers(data + nl_pos[2], nl_pos[1] - nl_pos[2]);

      //printf(">isCodeBlock local_indent %d>=%d+4=%d\n",
      //    indent0,indent2,indent0>=indent2+4);
      // if the difference is >4 spaces -> code block
      return indent0 >= indent + codeBlockIndent;
   } else { // not enough lines to determine the relative indent, use global indent
      // check that line -1 is empty
      if (nl == 1 && !isEmptyLine(data - offset, offset - 1)) {
         return false;
      }
      //printf(">isCodeBlock global indent %d>=%d+4=%d nl=%d\n",
      //    indent0,indent,indent0>=indent+4,nl);
      return indent0 >= indent + codeBlockIndent;
   }
}

/** Finds the location of the table's contains in the string \a data.
 *  Only one line will be inspected.
 *  @param[in] data pointer to the string buffer.
 *  @param[in] size the size of the buffer.
 *  @param[out] start offset of the first character of the table content
 *  @param[out] end   offset of the last character of the table content
 *  @param[out] columns number of table columns found
 *  @returns The offset until the next line in the buffer.
 */
int findTableColumns(const char *data, int size, int &start, int &end, int &columns)
{
   int i = 0, n = 0;
   int eol;
   // find start character of the table line
   while (i < size && data[i] == ' ') {
      i++;
   }
   if (i < size && data[i] == '|' && data[i] != '\n') {
      i++, n++;   // leading | does not count
   }
   start = i;

   // find end character of the table line
   while (i < size && data[i] != '\n') {
      i++;
   }
   eol = i + 1;
   i--;
   while (i > 0 && data[i] == ' ') {
      i--;
   }
   if (i > 0 && data[i - 1] != '\\' && data[i] == '|') {
      i--, n++;   // trailing or escaped | does not count
   }
   end = i;

   // count columns between start and end
   columns = 0;
   if (end > start) {
      i = start;
      while (i <= end) { // look for more column markers
         if (data[i] == '|' && (i == 0 || data[i - 1] != '\\')) {
            columns++;
         }
         if (columns == 1) {
            columns++;   // first | make a non-table into a two column table
         }
         i++;
      }
   }
   if (n == 2 && columns == 0) { // table row has | ... |
      columns++;
   }
   //printf("findTableColumns(start=%d,end=%d,columns=%d) eol=%d\n",
   //    start,end,columns,eol);
   return eol;
}

/** Returns true iff data points to the start of a table block */
static bool isTableBlock(const char *data, int size)
{
   int cc0, start, end;

   // the first line should have at least two columns separated by '|'
   int i = findTableColumns(data, size, start, end, cc0);
   if (i >= size || cc0 < 1) {
      //printf("isTableBlock: no |'s in the header\n");
      return false;
   }

   int cc1;
   int ret = findTableColumns(data + i, size - i, start, end, cc1);
   int j = i + start;
   // separator line should consist of |, - and : and spaces only
   while (j <= end + i) {
      if (data[j] != ':' && data[j] != '-' && data[j] != '|' && data[j] != ' ') {
         //printf("isTableBlock: invalid character '%c'\n",data[j]);
         return false; // invalid characters in table separator
      }
      j++;
   }
   if (cc1 != cc0) { // number of columns should be same as previous line
      return false;
   }

   i += ret; // goto next line
   int cc2;
   findTableColumns(data + i, size - i, start, end, cc2);

   //printf("isTableBlock: %d\n",cc1==cc2);
   return cc1 == cc2;
}

static int writeTableBlock(GrowBuf &out, const char *data, int size)
{
   int i = 0, j, k;
   int columns, start, end, cc;

   i = findTableColumns(data, size, start, end, columns);

   out.addStr("<table>");

   // write table header, in range [start..end]
   out.addStr("<tr>");

   int headerStart = start;
   int headerEnd = end;

   // read cell alignments
   int ret = findTableColumns(data + i, size - i, start, end, cc);
   k = 0;
   Alignment *columnAlignment = new Alignment[columns];

   bool leftMarker = false, rightMarker = false;
   bool startFound = false;
   j = start + i;
   while (j <= end + i) {
      if (!startFound) {
         if (data[j] == ':') {
            leftMarker = true;
            startFound = true;
         }
         if (data[j] == '-') {
            startFound = true;
         }
         //printf("  data[%d]=%c startFound=%d\n",j,data[j],startFound);
      }
      if      (data[j] == '-') {
         rightMarker = false;
      } else if (data[j] == ':') {
         rightMarker = true;
      }
      if (j <= end + i && (data[j] == '|' && (j == 0 || data[j - 1] != '\\'))) {
         if (k < columns) {
            columnAlignment[k] = markersToAlignment(leftMarker, rightMarker);
            //printf("column[%d] alignment=%d\n",k,columnAlignment[k]);
            leftMarker = false;
            rightMarker = false;
            startFound = false;
         }
         k++;
      }
      j++;
   }
   if (k < columns) {
      columnAlignment[k] = markersToAlignment(leftMarker, rightMarker);
      //printf("column[%d] alignment=%d\n",k,columnAlignment[k]);
   }
   // proceed to next line
   i += ret;

   int m = headerStart;
   for (k = 0; k < columns; k++) {
      out.addStr("<th");
      switch (columnAlignment[k]) {
         case AlignLeft:
            out.addStr(" align=\"left\"");
            break;
         case AlignRight:
            out.addStr(" align=\"right\"");
            break;
         case AlignCenter:
            out.addStr(" align=\"center\"");
            break;
         case AlignNone:
            break;
      }
      out.addStr(">");
      while (m <= headerEnd && (data[m] != '|' || (m > 0 && data[m - 1] == '\\'))) {
         out.addChar(data[m++]);
      }
      m++;
   }
   out.addStr("\n</th>\n");

   // write table cells
   while (i < size) {
      int ret = findTableColumns(data + i, size - i, start, end, cc);
      //printf("findTableColumns cc=%d\n",cc);
      if (cc != columns) {
         break;   // end of table
      }

      out.addStr("<tr>");
      j = start + i;
      int columnStart = j;
      k = 0;
      while (j <= end + i) {
         if (j == columnStart) {
            out.addStr("<td");
            switch (columnAlignment[k]) {
               case AlignLeft:
                  out.addStr(" align=\"left\"");
                  break;
               case AlignRight:
                  out.addStr(" align=\"right\"");
                  break;
               case AlignCenter:
                  out.addStr(" align=\"center\"");
                  break;
               case AlignNone:
                  break;
            }
            out.addStr(">");
         }
         if (j <= end + i && (data[j] == '|' && (j == 0 || data[j - 1] != '\\'))) {
            columnStart = j + 1;
            k++;
         } else {
            out.addChar(data[j]);
         }
         j++;
      }
      out.addChar('\n');

      // proceed to next line
      i += ret;
   }

   out.addStr("</table> ");

   delete[] columnAlignment;

   return i;
}

void writeOneLineHeaderOrRuler(GrowBuf &out, const char *data, int size)
{
   int level;

   QByteArray header;
   QByteArray id;

   if (isHRuler(data, size)) {
      out.addStr("<hr>\n");

   } else if ((level = isAtxHeader(data, size, header, id))) {
      
      QString hTag;

      if (level < 5 && ! id.isEmpty()) {

         SectionInfo::SectionType type = SectionInfo::Anchor;

         switch (level) {
            case 1:
               out.addStr("@section ");
               type = SectionInfo::Section;
               break;
            case 2:
               out.addStr("@subsection ");
               type = SectionInfo::Subsection;
               break;
            case 3:
               out.addStr("@subsubsection ");
               type = SectionInfo::Subsubsection;
               break;
            default:
               out.addStr("@paragraph ");
               type = SectionInfo::Paragraph;
               break;
         }

         out.addStr(id);
         out.addStr(" ");
         out.addStr(header);
         out.addStr("\n");

         QSharedPointer<SectionInfo> si = Doxygen::sectionDict->find(id);

         if (si) {
            if (si->lineNr != -1) {
               warn(g_fileName, g_lineNr, "multiple use of section label '%s', (first occurrence: %s, line %d)", 
                    header.data(), si->fileName.data(), si->lineNr);

            } else {
               warn(g_fileName, g_lineNr, "multiple use of section label '%s', (first occurrence: %s)", 
                    header.data(), si->fileName.data());
            }

         } else {
            si = QSharedPointer<SectionInfo> (new SectionInfo(g_fileName, g_lineNr, id, header, type, level));

            if (g_current) {
               g_current->anchors->append(*(si.data()));
            }

            Doxygen::sectionDict->insert(id, si);
         }

      } else {
         if (!id.isEmpty()) {
            out.addStr("\\anchor " + id + "\n");
         }
         
         hTag = QString("h%1").arg(level);

         out.addStr("<" + hTag.toUtf8() + ">");         

         out.addStr(header);
         out.addStr("</" + hTag.toUtf8() + ">\n");      
      }

   } else { // nothing interesting -> just output the line
      out.addStr(data, size);
   }
}

static int writeBlockQuote(GrowBuf &out, const char *data, int size)
{
   int l;
   int i = 0;
   int curLevel = 0;
   int end = 0;

   while (i < size) {
      // find end of this line
      end = i + 1;
      while (end <= size && data[end - 1] != '\n') {
         end++;
      }
      int j = i;
      int level = 0;
      int indent = i;
      // compute the quoting level
      while (j < end && (data[j] == ' ' || data[j] == '>')) {
         if (data[j] == '>') {
            level++;
            indent = j + 1;
         } else if (j > 0 && data[j - 1] == '>') {
            indent = j + 1;
         }
         j++;
      }
      if (j > 0 && data[j - 1] == '>' &&
            !(j == size || data[j] == '\n')) { // disqualify last > if not followed by space
         indent--;
         j--;
      }
      if (level > curLevel) { // quote level increased => add start markers
         for (l = curLevel; l < level; l++) {
            out.addStr("<blockquote>\n");
         }
      } else if (level < curLevel) { // quote level descreased => add end markers
         for (l = level; l < curLevel; l++) {
            out.addStr("</blockquote>\n");
         }
      }
      curLevel = level;
      if (level == 0) {
         break;   // end of quote block
      }
      // copy line without quotation marks
      out.addStr(data + indent, end - indent);
      // proceed with next line
      i = end;
   }
   // end of comment within blockquote => add end markers
   for (l = 0; l < curLevel; l++) {
      out.addStr("</blockquote>\n");
   }
   return i;
}

static int writeCodeBlock(GrowBuf &out, const char *data, int size, int refIndent)
{
   int i = 0, end;
   //printf("writeCodeBlock: data={%s}\n",QByteArray(data).left(size).data());
   out.addStr("@verbatim\n");
   int emptyLines = 0;
   while (i < size) {
      // find end of this line
      end = i + 1;
      while (end <= size && data[end - 1] != '\n') {
         end++;
      }
      int j = i;
      int indent = 0;
      while (j < end && data[j] == ' ') {
         j++, indent++;
      }
      //printf("j=%d end=%d indent=%d refIndent=%d tabSize=%d data={%s}\n",
      //    j,end,indent,refIndent,Config_getInt("TAB_SIZE"),QByteArray(data+i).left(end-i-1).data());
      if (j == end - 1) { // empty line
         emptyLines++;
         i = end;
      } else if (indent >= refIndent + codeBlockIndent) { // enough indent to contine the code block
         while (emptyLines > 0) { // write skipped empty lines
            // add empty line
            out.addStr("\n");
            emptyLines--;
         }
         // add code line minus the indent
         out.addStr(data + i + refIndent + codeBlockIndent, end - i - refIndent - codeBlockIndent);
         i = end;
      } else { // end of code block
         break;
      }
   }
   out.addStr("@endverbatim\n");
   while (emptyLines > 0) { // write skipped empty lines
      // add empty line
      out.addStr("\n");
      emptyLines--;
   }
   //printf("i=%d\n",i);
   return i;
}

// start searching for the end of the line start at offset \a i
// keeping track of possible blocks that need to to skipped.
static void findEndOfLine(GrowBuf &out, const char *data, int size,
                          int &pi, int &i, int &end)
{
   // find end of the line
   int nb = 0;
   end = i + 1;
   while (end <= size && data[end - 1] != '\n') {
      // while looking for the end of the line we might encounter a block
      // that needs to be passed unprocessed.
      if ((data[end - 1] == '\\' || data[end - 1] == '@') &&  // command
            (end <= 1 || (data[end - 2] != '\\' && data[end - 2] != '@')) // not escaped
         ) {
         QByteArray endBlockName = isBlockCommand(data + end - 1, end - 1, size - (end - 1));
         end++;
         if (!endBlockName.isEmpty()) {
            int l = endBlockName.length();
            for (; end < size - l - 1; end++) { // search for end of block marker
               if ((data[end] == '\\' || data[end] == '@') &&
                     data[end - 1] != '\\' && data[end - 1] != '@'
                  ) {
                  if (qstrncmp(&data[end + 1], endBlockName, l) == 0) {
                     if (pi != -1) { // output previous line if available
                        //printf("feol out={%s}\n",QByteArray(data+pi).left(i-pi).data());
                        out.addStr(data + pi, i - pi);
                     }
                     // found end marker, skip over this block
                     //printf("feol.block out={%s}\n",QByteArray(data+i).left(end+l+1-i).data());
                     out.addStr(data + i, end + l + 1 - i);
                     pi = -1;
                     i = end + l + 1; // continue after block
                     end = i + 1;
                     break;
                  }
               }
            }
         }
      } else if (nb == 0 && data[end - 1] == '<' && end < size - 6 &&
                 (end <= 1 || (data[end - 2] != '\\' && data[end - 2] != '@'))
                ) {
         if (tolower(data[end]) == 'p' && tolower(data[end + 1]) == 'r' &&
               tolower(data[end + 2]) == 'e' && data[end + 3] == '>') { // <pre> tag

            if (pi != -1) { // output previous line if available
               out.addStr(data + pi, i - pi);
            }

            // output part until <pre>
            out.addStr(data + i, end - 1 - i);
            // output part until </pre>
            i = end - 1 + processHtmlTag(out, data + end - 1, end - 1, size - end + 1);
            pi = -1;
            end = i + 1;
            break;
         } else {
            end++;
         }
      } else if (nb == 0 && data[end - 1] == '`') {
         while (end <= size && data[end - 1] == '`') {
            end++, nb++;
         }
      } else if (nb > 0 && data[end - 1] == '`') {
         int enb = 0;
         while (end <= size && data[end - 1] == '`') {
            end++, enb++;
         }
         if (enb == nb) {
            nb = 0;
         }
      } else {
         end++;
      }
   }
   //printf("findEndOfLine pi=%d i=%d end=%d {%s}\n",pi,i,end,QByteArray(data+i).left(end-i).data());
}

static void writeFencedCodeBlock(GrowBuf &out, const char *data, const char *lng,
                                 int blockStart, int blockEnd)
{
   QByteArray lang = lng;
   if (!lang.isEmpty() && lang.at(0) == '.') {
      lang = lang.mid(1);
   }
   out.addStr("@code");
   if (!lang.isEmpty()) {
      out.addStr("{" + lang + "}");
   }
   out.addStr(data + blockStart, blockEnd - blockStart);
   out.addStr("\n");
   out.addStr("@endcode");
}

static QByteArray processQuotations(const QByteArray &s, int refIndent)
{
   GrowBuf out;
   const char *data = s.data();
   int size = s.length();
   int i = 0, end = 0, pi = -1;
   int blockStart, blockEnd, blockOffset;
   QByteArray lang;
   while (i < size) {
      findEndOfLine(out, data, size, pi, i, end);
      // line is now found at [i..end)

      if (pi != -1) {
         if (isFencedCodeBlock(data + pi, size - pi, refIndent, lang, blockStart, blockEnd, blockOffset)) {
            writeFencedCodeBlock(out, data + pi, lang, blockStart, blockEnd);
            i = pi + blockOffset;
            pi = -1;
            end = i + 1;
            continue;
         } else if (isBlockQuote(data + pi, i - pi, refIndent)) {
            i = pi + writeBlockQuote(out, data + pi, size - pi);
            pi = -1;
            end = i + 1;
            continue;
         } else {
            //printf("quote out={%s}\n",QByteArray(data+pi).left(i-pi).data());
            out.addStr(data + pi, i - pi);
         }
      }
      pi = i;
      i = end;
   }
   if (pi != -1 && pi < size) { // deal with the last line
      if (isBlockQuote(data + pi, size - pi, refIndent)) {
         writeBlockQuote(out, data + pi, size - pi);
      } else {
         out.addStr(data + pi, size - pi);
      }
   }
   out.addChar(0);

   //printf("Process quotations\n---- input ----\n%s\n---- output ----\n%s\n------------\n",
   //    s.data(),out.get());

   return out.get();
}

static QByteArray processBlocks(const QByteArray &s, int indent)
{
   GrowBuf out;
   const char *data = s.data();
   int size = s.length();
   int i = 0, end = 0, pi = -1, ref, level;
   QByteArray id, link, title;
   int blockIndent = indent;

   // get indent for the first line
   end = i + 1;
   int sp = 0;
   while (end <= size && data[end - 1] != '\n') {
      if (data[end - 1] == ' ') {
         sp++;
      }
      end++;
   }

#if 0 // commented out, since starting with a comment block is probably a usage error
   // see also http://stackoverflow.com/q/20478611/784672

   // special case when the documentation starts with a code block
   // since the first line is skipped when looking for a code block later on.
   if (end > codeBlockIndent && isCodeBlock(data, 0, end, blockIndent)) {
      i = writeCodeBlock(out, data, size, blockIndent);
      end = i + 1;
      pi = -1;
   }
#endif

   // process each line
   while (i < size) {
      findEndOfLine(out, data, size, pi, i, end);

      // line is now found at [i..end)
      
      if (pi != -1) {
         int blockStart, blockEnd, blockOffset;

         QByteArray lang;
         blockIndent = indent;
         
         if ((level = isHeaderline(data + i, size - i)) > 0) {            

            while (pi < size && data[pi] == ' ') {
               pi++;
            }

            QByteArray header, id;
            convertStringFragment(header, data + pi, i - pi - 1);
            id = extractTitleId(header);

            if (!header.isEmpty()) {
               if (!id.isEmpty()) {
                  out.addStr(level == 1 ? "@section " : "@subsection ");
                  out.addStr(id);
                  out.addStr(" ");
                  out.addStr(header);
                  out.addStr("\n\n");

                  QSharedPointer<SectionInfo> si (Doxygen::sectionDict->find(id));

                  if (si) {
                     if (si->lineNr != -1) {
                        warn(g_fileName, g_lineNr, "multiple use of section label '%s', (first occurrence: %s, line %d)", 
                             header.data(), si->fileName.data(), si->lineNr);

                     } else {
                        warn(g_fileName, g_lineNr, "multiple use of section label '%s', (first occurrence: %s)", 
                             header.data(), si->fileName.data());
                     }

                  } else {
                     si = QSharedPointer<SectionInfo> (new SectionInfo(g_fileName, g_lineNr, id, header, 
                                          level == 1 ? SectionInfo::Section : SectionInfo::Subsection, level));

                     if (g_current) {
                        g_current->anchors->append(*si); 
                     }

                     Doxygen::sectionDict->insert(id, si);
                  }

               } else {
                  out.addStr(level == 1 ? "<h1>" : "<h2>");
                  out.addStr(header);
                  out.addStr(level == 1 ? "\n</h1>\n" : "\n</h2>\n");
               }

            } else {
               out.addStr("<hr>\n");
            }

            pi = -1;
            i = end;
            end = i + 1;
            continue;

         } else if ((ref = isLinkRef(data + pi, size - pi, id, link, title))) {
            //printf("found link ref: id='%s' link='%s' title='%s'\n",
            //       id.data(),link.data(),title.data());

            g_linkRefs.insert(id.toLower(), LinkRef(link, title));

            i   = ref + pi;
            pi  = -1;
            end = i + 1;

         } else if (isFencedCodeBlock(data + pi, size - pi, indent, lang, blockStart, blockEnd, blockOffset)) {
            //printf("Found FencedCodeBlock lang='%s' start=%d end=%d code={%s}\n",
            //       lang.data(),blockStart,blockEnd,QByteArray(data+pi+blockStart).left(blockEnd-blockStart).data());

            writeFencedCodeBlock(out, data + pi, lang, blockStart, blockEnd);
            i = pi + blockOffset;
            pi = -1;
            end = i + 1;
            continue;
         } else if (isCodeBlock(data + i, i, end - i, blockIndent)) {
            // skip previous line (it is empty anyway)
            i += writeCodeBlock(out, data + i, size - i, blockIndent);
            pi = -1;
            end = i + 1;
            continue;
         } else if (isTableBlock(data + pi, size - pi)) {
            i = pi + writeTableBlock(out, data + pi, size - pi);
            pi = -1;
            end = i + 1;
            continue;
         } else {
            writeOneLineHeaderOrRuler(out, data + pi, i - pi);
         }
      }

      pi = i;
      i = end;
   }

   //printf("last line %d size=%d\n",i,size);
   if (pi != -1 && pi < size) { // deal with the last line

      if (isLinkRef(data + pi, size - pi, id, link, title)) {         
         g_linkRefs.insert(id.toLower(), LinkRef(link, title));

      } else {
         writeOneLineHeaderOrRuler(out, data + pi, size - pi);
      }
   }

   out.addChar(0);
   return out.get();
}

static QByteArray extractPageTitle(QByteArray &docs, QByteArray &id)
{
   int ln = 0;
   // first first non-empty line

   QByteArray title;
   const char *data = docs.data();
   int i = 0;
   int size = docs.size();

   while (i < size && (data[i] == ' ' || data[i] == '\n')) {
      if (data[i] == '\n') {
         ln++;
      }
      i++;
   }

   if (i >= size) {
      return "";
   }

   int end1 = i + 1;

   while (end1 < size && data[end1 - 1] != '\n') {
      end1++;
   }

   //printf("i=%d end1=%d size=%d line='%s'\n",i,end1,size,docs.mid(i,end1-i).data());
   // first line from i..end1
   if (end1 < size) {
      ln++;
      // second line form end1..end2
      int end2 = end1 + 1;
     
 while (end2 < size && data[end2 - 1] != '\n') {
         end2++;
      }
      if (isHeaderline(data + end1, size - end1)) {
         convertStringFragment(title, data + i, end1 - i - 1);
         QByteArray lns;
         lns.fill('\n', ln);
         docs = lns + docs.mid(end2);
         id = extractTitleId(title);
         //printf("extractPageTitle(title='%s' docs='%s' id='%s')\n",title.data(),docs.data(),id.data());
         return title;
      }
   }
   if (i < end1 && isAtxHeader(data + i, end1 - i, title, id) > 0) {
      docs = docs.mid(end1);
   }
   //printf("extractPageTitle(title='%s' docs='%s' id='%s')\n",title.data(),docs.data(),id.data());
   return title;
}

static QByteArray detab(const QByteArray &s, int &refIndent)
{
   static int tabSize = Config_getInt("TAB_SIZE");
   GrowBuf out;
   int size = s.length();
   const char *data = s.data();
   int i = 0;
   int col = 0;
   const int maxIndent = 1000000; // value representing infinity
   int minIndent = maxIndent;
   while (i < size) {
      char c = data[i++];
      switch (c) {
         case '\t': { // expand tab
            int stop = tabSize - (col % tabSize);
            //printf("expand at %d stop=%d\n",col,stop);
            col += stop;
            while (stop--) {
               out.addChar(' ');
            }
         }
         break;
         case '\n': // reset colomn counter
            out.addChar(c);
            col = 0;
            break;
         case ' ': // increment column counter
            out.addChar(c);
            col++;
            break;
         default: // non-whitespace => update minIndent
            out.addChar(c);
            if (c < 0 && i < size) { // multibyte sequence
               out.addChar(data[i++]); // >= 2 bytes
               if (((uchar)c & 0xE0) == 0xE0 && i < size) {
                  out.addChar(data[i++]); // 3 bytes
               }
               if (((uchar)c & 0xF0) == 0xF0 && i < size) {
                  out.addChar(data[i++]); // 4 byres
               }
            }
            if (col < minIndent) {
               minIndent = col;
            }
            col++;
      }
   }
   if (minIndent != maxIndent) {
      refIndent = minIndent;
   } else {
      refIndent = 0;
   }
   out.addChar(0);
   //printf("detab refIndent=%d\n",refIndent);
   return out.get();
}

//---------------------------------------------------------------------------

QByteArray processMarkdown(const QByteArray &fileName, const int lineNr, Entry *e, const QByteArray &input)
{
   static bool init = false;
   if (!init) {
      // setup callback table for special characters
      g_actions[(unsigned int)'_'] = processEmphasis;
      g_actions[(unsigned int)'*'] = processEmphasis;
      g_actions[(unsigned int)'`'] = processCodeSpan;
      g_actions[(unsigned int)'\\'] = processSpecialCommand;
      g_actions[(unsigned int)'@'] = processSpecialCommand;
      g_actions[(unsigned int)'['] = processLink;
      g_actions[(unsigned int)'!'] = processLink;
      g_actions[(unsigned int)'<'] = processHtmlTag;
      g_actions[(unsigned int)'-'] = processNmdash;
      g_actions[(unsigned int)'"'] = processQuoted;
      init = true;
   }
 
   g_linkRefs.clear();
   g_current = e;
   g_fileName = fileName;
   g_lineNr   = lineNr;

   static GrowBuf out;
   if (input.isEmpty()) {
      return input;
   }

   out.clear();
   int refIndent;

   // for replace tabs by spaces
   QByteArray s = detab(input, refIndent);

   //printf("======== DeTab =========\n---- output -----\n%s\n---------\n",s.data());
   // then process quotation blocks (as these may contain other blocks)
   s = processQuotations(s, refIndent);

   //printf("======== Quotations =========\n---- output -----\n%s\n---------\n",s.data());
   // then process block items (headers, rules, and code blocks, references)
   s = processBlocks(s, refIndent);

   //printf("======== Blocks =========\n---- output -----\n%s\n---------\n",s.data());
   // finally process the inline markup (links, emphasis and code spans)
   processInline(out, s, s.length());

   out.addChar(0);
   Debug::print(Debug::Markdown, 0, "======== Markdown =========\n---- input ------- \n%s\n---- output -----\n%s\n---------\n", input.data(), out.get());
   return out.get();
}

//---------------------------------------------------------------------------

QByteArray markdownFileNameToId(const QByteArray &fileName)
{
   QByteArray baseFn  = stripFromPath(QFileInfo(fileName).absoluteFilePath().toUtf8()).toUtf8();

   int i = baseFn.lastIndexOf('.');
   if (i != -1) {
      baseFn = baseFn.left(i);
   }

   QByteArray baseName = substitute(substitute(baseFn, " ", "_"), "/", "_");

   return "md_" + baseName;
}

void MarkdownFileParser::parseInput(const char *fileName, const char *fileBuf, Entry *root,
                                    bool /*sameTranslationUnit*/, QStringList & /*filesInSameTranslationUnit*/)
{
   Entry *current = new Entry;
   current->lang = SrcLangExt_Markdown;
   current->fileName = fileName;
   current->docFile  = fileName;
   current->docLine  = 1;

   QByteArray docs = fileBuf;
   QByteArray id;
   QByteArray title = extractPageTitle(docs, id).trimmed();
   QByteArray titleFn = QFileInfo(fileName).baseName().toUtf8();
   QByteArray fn      = QFileInfo(fileName).fileName().toUtf8();
   static QByteArray mdfileAsMainPage = Config_getString("USE_MDFILE_AS_MAINPAGE");
   if (id.isEmpty()) {
      id = markdownFileNameToId(fileName);
   }
   if (title.isEmpty()) {
      title = titleFn;
   }
   if (!mdfileAsMainPage.isEmpty() &&
         (fn == mdfileAsMainPage || // name reference
          QFileInfo(fileName).absoluteFilePath() ==
          QFileInfo(mdfileAsMainPage).absoluteFilePath()) // file reference with path
      ) {
      docs.prepend("@mainpage\n");
   } else if (id == "mainpage" || id == "index") {
      docs.prepend("@mainpage " + title + "\n");
   } else {
      docs.prepend("@page " + id + " " + title + "\n");
   }
   int lineNr = 1;
   int position = 0;

   // even without markdown support enabled, we still
   // parse markdown files as such
   bool markdownEnabled = Doxygen::markdownSupport;
   Doxygen::markdownSupport = true;

   bool needsEntry = false;
   Protection prot = Public;

// BROOM 

/*
   while (parseCommentBlock(this, current, docs, fileName,lineNr,
             false,     // isBrief
             false,     // javadoc autobrief
             false,     // inBodyDocs
             prot,      // protection
             position,
             needsEntry)) {

      if (needsEntry) {
         QByteArray docFile = current->docFile;
         root->addSubEntry(current);
         current = new Entry;
         current->lang = SrcLangExt_Markdown;
         current->docFile = docFile;
         current->docLine = lineNr;
      }
   }

*/

   if (needsEntry) {
      root->addSubEntry(current);
   }

   // restore setting
   Doxygen::markdownSupport = markdownEnabled;
   //g_correctSectionLevel = false;
}

void MarkdownFileParser::parseCode(CodeOutputInterface &codeOutIntf,
                                   const char *scopeName,
                                   const QByteArray &input,
                                   SrcLangExt lang,
                                   bool isExampleBlock,
                                   const char *exampleName,
                                   FileDef *fileDef,
                                   int startLine,
                                   int endLine,
                                   bool inlineFragment,
                                   MemberDef *memberDef,
                                   bool showLineNumbers,
                                   Definition *searchCtx,
                                   bool collectXRefs
                                  )
{
   ParserInterface *pIntf = Doxygen::parserManager->getParser("*.cpp");
   if (pIntf != this) {
      pIntf->parseCode(
         codeOutIntf, scopeName, input, lang, isExampleBlock, exampleName,
         fileDef, startLine, endLine, inlineFragment, memberDef, showLineNumbers,
         searchCtx, collectXRefs);
   }
}

void MarkdownFileParser::resetCodeParserState()
{
   ParserInterface *pIntf = Doxygen::parserManager->getParser("*.cpp");
   if (pIntf != this) {
      pIntf->resetCodeParserState();
   }
}

void MarkdownFileParser::parsePrototype(const char *text)
{
   ParserInterface *pIntf = Doxygen::parserManager->getParser("*.cpp");
   if (pIntf != this) {
      pIntf->parsePrototype(text);
   }
}

