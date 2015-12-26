// -*- mode: cpp; mode: fold -*-
// Description								/*{{{*/
// $Id: tagfile.cc,v 1.37.2.2 2003/12/31 16:02:30 mdz Exp $
/* ######################################################################

   Fast scanner for RFC-822 type header information
   
   This uses a rotating buffer to load the package information into.
   The scanner runs over it and isolates and indexes a single section.
   
   ##################################################################### */
									/*}}}*/
// Include Files							/*{{{*/
#include<config.h>

#include <apt-pkg/tagfile.h>
#include <apt-pkg/error.h>
#include <apt-pkg/strutl.h>
#include <apt-pkg/fileutl.h>

#include <string>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <apti18n.h>
									/*}}}*/

using std::string;

class pkgTagFilePrivate
{
public:
   void Reset(FileFd * const pFd, unsigned long long const pSize)
   {
      if (Buffer != NULL)
	 free(Buffer);
      Buffer = NULL;
      Fd = pFd;
      Start = NULL;
      End = NULL;
      Done = false;
      iOffset = 0;
      Size = pSize;
   }

   pkgTagFilePrivate(FileFd * const pFd, unsigned long long const Size) : Buffer(NULL)
   {
      Reset(pFd, Size);
   }
   FileFd * Fd;
   char *Buffer;
   char *Start;
   char *End;
   bool Done;
   unsigned long long iOffset;
   unsigned long long Size;

   ~pkgTagFilePrivate()
   {
      if (Buffer != NULL)
	 free(Buffer);
   }
};

class pkgTagSectionPrivate
{
public:
   pkgTagSectionPrivate()
   {
   }
   struct TagData {
      unsigned int StartTag;
      unsigned int EndTag;
      unsigned int StartValue;
      unsigned int NextInBucket;

      explicit TagData(unsigned int const StartTag) : StartTag(StartTag), EndTag(0), StartValue(0), NextInBucket(0) {}
   };
   std::vector<TagData> Tags;
};

static unsigned long AlphaHash(const char *Text, size_t Length)		/*{{{*/
{
   /* This very simple hash function for the last 8 letters gives
      very good performance on the debian package files */
   if (Length > 8)
   {
    Text += (Length - 8);
    Length = 8;
   }
   unsigned long Res = 0;
   for (size_t i = 0; i < Length; ++i)
      Res = ((unsigned long)(Text[i]) & 0xDF) ^ (Res << 1);
   return Res & 0xFF;
}
									/*}}}*/

// TagFile::pkgTagFile - Constructor					/*{{{*/
// ---------------------------------------------------------------------
/* */
pkgTagFile::pkgTagFile(FileFd * const pFd,unsigned long long const Size)
   : d(new pkgTagFilePrivate(pFd, Size + 4))
{
   Init(pFd, Size);
}
void pkgTagFile::Init(FileFd * const pFd,unsigned long long Size)
{
   /* The size is increased by 4 because if we start with the Size of the
      filename we need to try to read 1 char more to see an EOF faster, 1
      char the end-pointer can be on and maybe 2 newlines need to be added
      to the end of the file -> 4 extra chars */
   Size += 4;
   d->Reset(pFd, Size);

   if (d->Fd->IsOpen() == false)
      d->Start = d->End = d->Buffer = 0;
   else
      d->Buffer = (char*)malloc(sizeof(char) * Size);

   if (d->Buffer == NULL)
      d->Done = true;
   else
      d->Done = false;

   d->Start = d->End = d->Buffer;
   d->iOffset = 0;
   if (d->Done == false)
      Fill();
}
									/*}}}*/
// TagFile::~pkgTagFile - Destructor					/*{{{*/
// ---------------------------------------------------------------------
/* */
pkgTagFile::~pkgTagFile()
{
   delete d;
}
									/*}}}*/
// TagFile::Offset - Return the current offset in the buffer		/*{{{*/
APT_PURE unsigned long pkgTagFile::Offset()
{
   return d->iOffset;
}
									/*}}}*/
// TagFile::Resize - Resize the internal buffer				/*{{{*/
// ---------------------------------------------------------------------
/* Resize the internal buffer (double it in size). Fail if a maximum size
 * size is reached.
 */
bool pkgTagFile::Resize()
{
   // fail is the buffer grows too big
   if(d->Size > 1024*1024+1)
      return false;

   return Resize(d->Size * 2);
}
bool pkgTagFile::Resize(unsigned long long const newSize)
{
   unsigned long long const EndSize = d->End - d->Start;

   // get new buffer and use it
   char* newBuffer = (char*)realloc(d->Buffer, sizeof(char) * newSize);
   if (newBuffer == NULL)
      return false;
   d->Buffer = newBuffer;
   d->Size = newSize;

   // update the start/end pointers to the new buffer
   d->Start = d->Buffer;
   d->End = d->Start + EndSize;
   return true;
}
									/*}}}*/
// TagFile::Step - Advance to the next section				/*{{{*/
// ---------------------------------------------------------------------
/* If the Section Scanner fails we refill the buffer and try again. 
 * If that fails too, double the buffer size and try again until a
 * maximum buffer is reached.
 */
bool pkgTagFile::Step(pkgTagSection &Tag)
{
   if(Tag.Scan(d->Start,d->End - d->Start) == false)
   {
      do
      {
	 if (Fill() == false)
	    return false;

	 if(Tag.Scan(d->Start,d->End - d->Start, false))
	    break;

	 if (Resize() == false)
	    return _error->Error(_("Unable to parse package file %s (%d)"),
		  d->Fd->Name().c_str(), 1);

      } while (Tag.Scan(d->Start,d->End - d->Start, false) == false);
   }

   d->Start += Tag.size();
   d->iOffset += Tag.size();

   Tag.Trim();
   return true;
}
									/*}}}*/
// TagFile::Fill - Top up the buffer					/*{{{*/
// ---------------------------------------------------------------------
/* This takes the bit at the end of the buffer and puts it at the start
   then fills the rest from the file */
bool pkgTagFile::Fill()
{
   unsigned long long EndSize = d->End - d->Start;
   unsigned long long Actual = 0;
   
   memmove(d->Buffer,d->Start,EndSize);
   d->Start = d->Buffer;
   d->End = d->Buffer + EndSize;
   
   if (d->Done == false)
   {
      // See if only a bit of the file is left
      unsigned long long const dataSize = d->Size - ((d->End - d->Buffer) + 1);
      if (d->Fd->Read(d->End, dataSize, &Actual) == false)
	 return false;
      if (Actual != dataSize)
	 d->Done = true;
      d->End += Actual;
   }
   
   if (d->Done == true)
   {
      if (EndSize <= 3 && Actual == 0)
	 return false;
      if (d->Size - (d->End - d->Buffer) < 4)
	 return true;
      
      // Append a double new line if one does not exist
      unsigned int LineCount = 0;
      for (const char *E = d->End - 1; E - d->End < 6 && (*E == '\n' || *E == '\r'); E--)
	 if (*E == '\n')
	    LineCount++;
      if (LineCount < 2)
      {
	 if ((unsigned)(d->End - d->Buffer) >= d->Size)
	    Resize(d->Size + 3);
	 for (; LineCount < 2; LineCount++)
	    *d->End++ = '\n';
      }
      
      return true;
   }
   
   return true;
}
									/*}}}*/
// TagFile::Jump - Jump to a pre-recorded location in the file		/*{{{*/
// ---------------------------------------------------------------------
/* This jumps to a pre-recorded file location and reads the record
   that is there */
bool pkgTagFile::Jump(pkgTagSection &Tag,unsigned long long Offset)
{
   // We are within a buffer space of the next hit..
   if (Offset >= d->iOffset && d->iOffset + (d->End - d->Start) > Offset)
   {
      unsigned long long Dist = Offset - d->iOffset;
      d->Start += Dist;
      d->iOffset += Dist;
      // if we have seen the end, don't ask for more
      if (d->Done == true)
	 return Tag.Scan(d->Start, d->End - d->Start);
      else
	 return Step(Tag);
   }

   // Reposition and reload..
   d->iOffset = Offset;
   d->Done = false;
   if (d->Fd->Seek(Offset) == false)
      return false;
   d->End = d->Start = d->Buffer;
   
   if (Fill() == false)
      return false;

   if (Tag.Scan(d->Start, d->End - d->Start) == true)
      return true;
   
   // This appends a double new line (for the real eof handling)
   if (Fill() == false)
      return false;
   
   if (Tag.Scan(d->Start, d->End - d->Start, false) == false)
      return _error->Error(_("Unable to parse package file %s (%d)"),d->Fd->Name().c_str(), 2);
   
   return true;
}
									/*}}}*/
// pkgTagSection::pkgTagSection - Constructor				/*{{{*/
// ---------------------------------------------------------------------
/* */
APT_IGNORE_DEPRECATED_PUSH
pkgTagSection::pkgTagSection()
   : Section(0), d(new pkgTagSectionPrivate()), Stop(0)
{
   memset(&AlphaIndexes, 0, sizeof(AlphaIndexes));
}
APT_IGNORE_DEPRECATED_POP
									/*}}}*/
// TagSection::Scan - Scan for the end of the header information	/*{{{*/
bool pkgTagSection::Scan(const char *Start,unsigned long MaxLength, bool const Restart)
{
   Section = Start;
   const char *End = Start + MaxLength;

   if (Restart == false && d->Tags.empty() == false)
   {
      Stop = Section + d->Tags.back().StartTag;
      if (End <= Stop)
	 return false;
      Stop = (const char *)memchr(Stop,'\n',End - Stop);
      if (Stop == NULL)
	 return false;
      ++Stop;
   }
   else
   {
      Stop = Section;
      if (d->Tags.empty() == false)
      {
	 memset(&AlphaIndexes, 0, sizeof(AlphaIndexes));
	 d->Tags.clear();
      }
      d->Tags.reserve(0x100);
   }
   unsigned int TagCount = d->Tags.size();

   if (Stop == 0)
      return false;

   pkgTagSectionPrivate::TagData lastTagData(0);
   lastTagData.EndTag = 0;
   unsigned long lastTagHash = 0;
   while (Stop < End)
   {
      TrimRecord(true,End);

      // this can happen when TrimRecord trims away the entire Record
      // (e.g. because it just contains comments)
      if(Stop == End)
         return true;

      // Start a new index and add it to the hash
      if (isspace_ascii(Stop[0]) == 0)
      {
	 // store the last found tag
	 if (lastTagData.EndTag != 0)
	 {
	    if (AlphaIndexes[lastTagHash] != 0)
	       lastTagData.NextInBucket = AlphaIndexes[lastTagHash];
	    APT_IGNORE_DEPRECATED_PUSH
	    AlphaIndexes[lastTagHash] = TagCount;
	    APT_IGNORE_DEPRECATED_POP
	    d->Tags.push_back(lastTagData);
	 }

	 APT_IGNORE_DEPRECATED(++TagCount;)
	 lastTagData = pkgTagSectionPrivate::TagData(Stop - Section);
	 // find the colon separating tag and value
	 char const * Colon = (char const *) memchr(Stop, ':', End - Stop);
	 if (Colon == NULL)
	    return false;
	 // find the end of the tag (which might or might not be the colon)
	 char const * EndTag = Colon;
	 --EndTag;
	 for (; EndTag > Stop && isspace_ascii(*EndTag) != 0; --EndTag)
	    ;
	 ++EndTag;
	 lastTagData.EndTag = EndTag - Section;
	 lastTagHash = AlphaHash(Stop, EndTag - Stop);
	 // find the beginning of the value
	 Stop = Colon + 1;
	 for (; isspace_ascii(*Stop) != 0; ++Stop);
	 if (Stop >= End)
	    return false;
	 lastTagData.StartValue = Stop - Section;
      }

      Stop = (const char *)memchr(Stop,'\n',End - Stop);

      if (Stop == 0)
	 return false;

      for (; Stop+1 < End && Stop[1] == '\r'; Stop++)
         /* nothing */
         ;

      // Double newline marks the end of the record
      if (Stop+1 < End && Stop[1] == '\n')
      {
	 if (lastTagData.EndTag != 0)
	 {
	    if (AlphaIndexes[lastTagHash] != 0)
	       lastTagData.NextInBucket = AlphaIndexes[lastTagHash];
	    APT_IGNORE_DEPRECATED(AlphaIndexes[lastTagHash] = TagCount;)
	    d->Tags.push_back(lastTagData);
	 }

	 pkgTagSectionPrivate::TagData const td(Stop - Section);
	 d->Tags.push_back(td);
	 TrimRecord(false,End);
	 return true;
      }
      
      Stop++;
   }

   return false;
}
									/*}}}*/
// TagSection::TrimRecord - Trim off any garbage before/after a record	/*{{{*/
// ---------------------------------------------------------------------
/* There should be exactly 2 newline at the end of the record, no more. */
void pkgTagSection::TrimRecord(bool BeforeRecord, const char*& End)
{
   if (BeforeRecord == true)
      return;
   for (; Stop < End && (Stop[0] == '\n' || Stop[0] == '\r'); Stop++);
}
									/*}}}*/
// TagSection::Trim - Trim off any trailing garbage			/*{{{*/
// ---------------------------------------------------------------------
/* There should be exactly 1 newline at the end of the buffer, no more. */
void pkgTagSection::Trim()
{
   for (; Stop > Section + 2 && (Stop[-2] == '\n' || Stop[-2] == '\r'); Stop--);
}
									/*}}}*/
// TagSection::Exists - return True if a tag exists			/*{{{*/
bool pkgTagSection::Exists(const char* const Tag) const
{
   unsigned int tmp;
   return Find(Tag, tmp);
}
									/*}}}*/
// TagSection::Find - Locate a tag					/*{{{*/
// ---------------------------------------------------------------------
/* This searches the section for a tag that matches the given string. */
bool pkgTagSection::Find(const char *Tag,unsigned int &Pos) const
{
   size_t const Length = strlen(Tag);
   unsigned int Bucket = AlphaIndexes[AlphaHash(Tag, Length)];
   if (Bucket == 0)
      return false;

   for (; Bucket != 0; Bucket = d->Tags[Bucket - 1].NextInBucket)
   {
      if ((d->Tags[Bucket - 1].EndTag - d->Tags[Bucket - 1].StartTag) != Length)
	 continue;

      char const * const St = Section + d->Tags[Bucket - 1].StartTag;
      if (strncasecmp(Tag,St,Length) != 0)
	 continue;

      Pos = Bucket - 1;
      return true;
   }

   Pos = 0;
   return false;
}
bool pkgTagSection::Find(const char *Tag,const char *&Start,
		         const char *&End) const
{
   unsigned int Pos;
   if (Find(Tag, Pos) == false)
      return false;

   Start = Section + d->Tags[Pos].StartValue;
   // Strip off the gunk from the end
   End = Section + d->Tags[Pos + 1].StartTag;
   if (unlikely(Start > End))
      return _error->Error("Internal parsing error");

   for (; isspace_ascii(End[-1]) != 0 && End > Start; --End);

   return true;
}
									/*}}}*/
// TagSection::FindS - Find a string					/*{{{*/
string pkgTagSection::FindS(const char *Tag) const
{
   const char *Start;
   const char *End;
   if (Find(Tag,Start,End) == false)
      return string();
   return string(Start,End);      
}
									/*}}}*/
// TagSection::FindRawS - Find a string					/*{{{*/
string pkgTagSection::FindRawS(const char *Tag) const
{
   unsigned int Pos;
   if (Find(Tag, Pos) == false)
      return "";

   char const *Start = (char const *) memchr(Section + d->Tags[Pos].EndTag, ':', d->Tags[Pos].StartValue - d->Tags[Pos].EndTag);
   ++Start;
   char const *End = Section + d->Tags[Pos + 1].StartTag;
   if (unlikely(Start > End))
      return "";

   for (; isspace_ascii(End[-1]) != 0 && End > Start; --End);

   return std::string(Start, End - Start);
}
									/*}}}*/
// TagSection::FindI - Find an integer					/*{{{*/
// ---------------------------------------------------------------------
/* */
signed int pkgTagSection::FindI(const char *Tag,signed long Default) const
{
   const char *Start;
   const char *Stop;
   if (Find(Tag,Start,Stop) == false)
      return Default;

   // Copy it into a temp buffer so we can use strtol
   char S[300];
   if ((unsigned)(Stop - Start) >= sizeof(S))
      return Default;
   strncpy(S,Start,Stop-Start);
   S[Stop - Start] = 0;

   errno = 0;
   char *End;
   signed long Result = strtol(S,&End,10);
   if (errno == ERANGE ||
       Result < std::numeric_limits<int>::min() || Result > std::numeric_limits<int>::max()) {
      errno = ERANGE;
      _error->Error(_("Cannot convert %s to integer: out of range"), S);
   }
   if (S == End)
      return Default;
   return Result;
}
									/*}}}*/
// TagSection::FindULL - Find an unsigned long long integer		/*{{{*/
// ---------------------------------------------------------------------
/* */
unsigned long long pkgTagSection::FindULL(const char *Tag, unsigned long long const &Default) const
{
   const char *Start;
   const char *Stop;
   if (Find(Tag,Start,Stop) == false)
      return Default;

   // Copy it into a temp buffer so we can use strtoull
   char S[100];
   if ((unsigned)(Stop - Start) >= sizeof(S))
      return Default;
   strncpy(S,Start,Stop-Start);
   S[Stop - Start] = 0;
   
   char *End;
   unsigned long long Result = strtoull(S,&End,10);
   if (S == End)
      return Default;
   return Result;
}
									/*}}}*/
// TagSection::FindB - Find boolean value                		/*{{{*/
// ---------------------------------------------------------------------
/* */
bool pkgTagSection::FindB(const char *Tag, bool const &Default) const
{
   const char *Start, *Stop;
   if (Find(Tag, Start, Stop) == false)
      return Default;
   return StringToBool(string(Start, Stop));
}
									/*}}}*/
// TagSection::FindFlag - Locate a yes/no type flag			/*{{{*/
// ---------------------------------------------------------------------
/* The bits marked in Flag are masked on/off in Flags */
bool pkgTagSection::FindFlag(const char * const Tag, uint8_t &Flags,
			     uint8_t const Flag) const
{
   const char *Start;
   const char *Stop;
   if (Find(Tag,Start,Stop) == false)
      return true;
   return FindFlag(Flags, Flag, Start, Stop);
}
bool pkgTagSection::FindFlag(uint8_t &Flags, uint8_t const Flag,
					char const* const Start, char const* const Stop)
{
   switch (StringToBool(string(Start, Stop)))
   {
      case 0:
      Flags &= ~Flag;
      return true;

      case 1:
      Flags |= Flag;
      return true;

      default:
      _error->Warning("Unknown flag value: %s",string(Start,Stop).c_str());
      return true;
   }
   return true;
}
bool pkgTagSection::FindFlag(const char *Tag,unsigned long &Flags,
			     unsigned long Flag) const
{
   const char *Start;
   const char *Stop;
   if (Find(Tag,Start,Stop) == false)
      return true;
   return FindFlag(Flags, Flag, Start, Stop);
}
bool pkgTagSection::FindFlag(unsigned long &Flags, unsigned long Flag,
					char const* Start, char const* Stop)
{
   switch (StringToBool(string(Start, Stop)))
   {
      case 0:
      Flags &= ~Flag;
      return true;

      case 1:
      Flags |= Flag;
      return true;

      default:
      _error->Warning("Unknown flag value: %s",string(Start,Stop).c_str());
      return true;
   }
   return true;
}
									/*}}}*/
void pkgTagSection::Get(const char *&Start,const char *&Stop,unsigned int I) const
{
   Start = Section + d->Tags[I].StartTag;
   Stop = Section + d->Tags[I+1].StartTag;
}
APT_PURE unsigned int pkgTagSection::Count() const {			/*{{{*/
   if (d->Tags.empty() == true)
      return 0;
   // the last element is just marking the end and isn't a real one
   return d->Tags.size() - 1;
}
									/*}}}*/
// TagSection::Write - Ordered (re)writing of fields			/*{{{*/
pkgTagSection::Tag pkgTagSection::Tag::Remove(std::string const &Name)
{
   return Tag(REMOVE, Name, "");
}
pkgTagSection::Tag pkgTagSection::Tag::Rename(std::string const &OldName, std::string const &NewName)
{
   return Tag(RENAME, OldName, NewName);
}
pkgTagSection::Tag pkgTagSection::Tag::Rewrite(std::string const &Name, std::string const &Data)
{
   if (Data.empty() == true)
      return Tag(REMOVE, Name, "");
   else
      return Tag(REWRITE, Name, Data);
}
static bool WriteTag(FileFd &File, std::string Tag, std::string const &Value)
{
   if (Value.empty() || isspace_ascii(Value[0]) != 0)
      Tag.append(":");
   else
      Tag.append(": ");
   Tag.append(Value);
   Tag.append("\n");
   return File.Write(Tag.c_str(), Tag.length());
}
static bool RewriteTags(FileFd &File, pkgTagSection const * const This, char const * const Tag,
      std::vector<pkgTagSection::Tag>::const_iterator &R,
      std::vector<pkgTagSection::Tag>::const_iterator const &REnd)
{
   size_t const TagLen = strlen(Tag);
   for (; R != REnd; ++R)
   {
      std::string data;
      if (R->Name.length() == TagLen && strncasecmp(R->Name.c_str(), Tag, R->Name.length()) == 0)
      {
	 if (R->Action != pkgTagSection::Tag::REWRITE)
	    break;
	 data = R->Data;
      }
      else if(R->Action == pkgTagSection::Tag::RENAME && R->Data.length() == TagLen &&
	    strncasecmp(R->Data.c_str(), Tag, R->Data.length()) == 0)
	 data = This->FindRawS(R->Name.c_str());
      else
	 continue;

      return WriteTag(File, Tag, data);
   }
   return true;
}
bool pkgTagSection::Write(FileFd &File, char const * const * const Order, std::vector<Tag> const &Rewrite) const
{
   // first pass: Write everything we have an order for
   if (Order != NULL)
   {
      for (unsigned int I = 0; Order[I] != 0; ++I)
      {
	 std::vector<Tag>::const_iterator R = Rewrite.begin();
	 if (RewriteTags(File, this, Order[I], R, Rewrite.end()) == false)
	    return false;
	 if (R != Rewrite.end())
	    continue;

	 if (Exists(Order[I]) == false)
	    continue;

	 if (WriteTag(File, Order[I], FindRawS(Order[I])) == false)
	    return false;
      }
   }
   // second pass: See if we have tags which aren't ordered
   if (d->Tags.empty() == false)
   {
      for (std::vector<pkgTagSectionPrivate::TagData>::const_iterator T = d->Tags.begin(); T != d->Tags.end() - 1; ++T)
      {
	 char const * const fieldname = Section + T->StartTag;
	 size_t fieldnamelen = T->EndTag - T->StartTag;
	 if (Order != NULL)
	 {
	    unsigned int I = 0;
	    for (; Order[I] != 0; ++I)
	    {
	       if (fieldnamelen == strlen(Order[I]) && strncasecmp(fieldname, Order[I], fieldnamelen) == 0)
		  break;
	    }
	    if (Order[I] != 0)
	       continue;
	 }

	 std::string const name(fieldname, fieldnamelen);
	 std::vector<Tag>::const_iterator R = Rewrite.begin();
	 if (RewriteTags(File, this, name.c_str(), R, Rewrite.end()) == false)
	    return false;
	 if (R != Rewrite.end())
	    continue;

	 if (WriteTag(File, name, FindRawS(name.c_str())) == false)
	    return false;
      }
   }
   // last pass: see if there are any rewrites remaining we haven't done yet
   for (std::vector<Tag>::const_iterator R = Rewrite.begin(); R != Rewrite.end(); ++R)
   {
      if (R->Action == Tag::REMOVE)
	 continue;
      std::string const name = ((R->Action == Tag::RENAME) ? R->Data : R->Name);
      if (Exists(name.c_str()))
	 continue;
      if (Order != NULL)
      {
	 unsigned int I = 0;
	 for (; Order[I] != 0; ++I)
	 {
	    if (strncasecmp(name.c_str(), Order[I], name.length()) == 0 && name.length() == strlen(Order[I]))
	       break;
	 }
	 if (Order[I] != 0)
	    continue;
      }

      if (WriteTag(File, name, ((R->Action == Tag::RENAME) ? FindRawS(R->Name.c_str()) : R->Data)) == false)
	 return false;
   }
   return true;
}
									/*}}}*/

void pkgUserTagSection::TrimRecord(bool /*BeforeRecord*/, const char* &End)/*{{{*/
{
   for (; Stop < End && (Stop[0] == '\n' || Stop[0] == '\r' || Stop[0] == '#'); Stop++)
      if (Stop[0] == '#')
	 Stop = (const char*) memchr(Stop,'\n',End-Stop);
}
									/*}}}*/

#include "tagfile-order.c"

// TFRewrite - Rewrite a control record					/*{{{*/
// ---------------------------------------------------------------------
/* This writes the control record to stdout rewriting it as necessary. The
   override map item specificies the rewriting rules to follow. This also
   takes the time to sort the feild list. */
APT_IGNORE_DEPRECATED_PUSH
bool TFRewrite(FILE *Output,pkgTagSection const &Tags,const char *Order[],
	       TFRewriteData *Rewrite)
{
   unsigned char Visited[256];   // Bit 1 is Order, Bit 2 is Rewrite
   for (unsigned I = 0; I != 256; I++)
      Visited[I] = 0;

   // Set new tag up as necessary.
   for (unsigned int J = 0; Rewrite != 0 && Rewrite[J].Tag != 0; J++)
   {
      if (Rewrite[J].NewTag == 0)
	 Rewrite[J].NewTag = Rewrite[J].Tag;
   }
   
   // Write all all of the tags, in order.
   if (Order != NULL)
   {
      for (unsigned int I = 0; Order[I] != 0; I++)
      {
         bool Rewritten = false;
         
         // See if this is a field that needs to be rewritten
         for (unsigned int J = 0; Rewrite != 0 && Rewrite[J].Tag != 0; J++)
         {
            if (strcasecmp(Rewrite[J].Tag,Order[I]) == 0)
            {
               Visited[J] |= 2;
               if (Rewrite[J].Rewrite != 0 && Rewrite[J].Rewrite[0] != 0)
               {
                  if (isspace_ascii(Rewrite[J].Rewrite[0]))
                     fprintf(Output,"%s:%s\n",Rewrite[J].NewTag,Rewrite[J].Rewrite);
                  else
                     fprintf(Output,"%s: %s\n",Rewrite[J].NewTag,Rewrite[J].Rewrite);
               }
               Rewritten = true;
               break;
            }
         }
	    
         // See if it is in the fragment
         unsigned Pos;
         if (Tags.Find(Order[I],Pos) == false)
            continue;
         Visited[Pos] |= 1;

         if (Rewritten == true)
            continue;
      
         /* Write out this element, taking a moment to rewrite the tag
            in case of changes of case. */
         const char *Start;
         const char *Stop;
         Tags.Get(Start,Stop,Pos);
      
         if (fputs(Order[I],Output) < 0)
            return _error->Errno("fputs","IO Error to output");
         Start += strlen(Order[I]);
         if (fwrite(Start,Stop - Start,1,Output) != 1)
            return _error->Errno("fwrite","IO Error to output");
         if (Stop[-1] != '\n')
            fprintf(Output,"\n");
      }
   }

   // Now write all the old tags that were missed.
   for (unsigned int I = 0; I != Tags.Count(); I++)
   {
      if ((Visited[I] & 1) == 1)
	 continue;

      const char *Start;
      const char *Stop;
      Tags.Get(Start,Stop,I);
      const char *End = Start;
      for (; End < Stop && *End != ':'; End++);

      // See if this is a field that needs to be rewritten
      bool Rewritten = false;
      for (unsigned int J = 0; Rewrite != 0 && Rewrite[J].Tag != 0; J++)
      {
	 if (stringcasecmp(Start,End,Rewrite[J].Tag) == 0)
	 {
	    Visited[J] |= 2;
	    if (Rewrite[J].Rewrite != 0 && Rewrite[J].Rewrite[0] != 0)
	    {
	       if (isspace_ascii(Rewrite[J].Rewrite[0]))
		  fprintf(Output,"%s:%s\n",Rewrite[J].NewTag,Rewrite[J].Rewrite);
	       else
		  fprintf(Output,"%s: %s\n",Rewrite[J].NewTag,Rewrite[J].Rewrite);
	    }
	    
	    Rewritten = true;
	    break;
	 }
      }      
      
      if (Rewritten == true)
	 continue;
      
      // Write out this element
      if (fwrite(Start,Stop - Start,1,Output) != 1)
	 return _error->Errno("fwrite","IO Error to output");
      if (Stop[-1] != '\n')
	 fprintf(Output,"\n");
   }
   
   // Now write all the rewrites that were missed
   for (unsigned int J = 0; Rewrite != 0 && Rewrite[J].Tag != 0; J++)
   {
      if ((Visited[J] & 2) == 2)
	 continue;
      
      if (Rewrite[J].Rewrite != 0 && Rewrite[J].Rewrite[0] != 0)
      {
	 if (isspace_ascii(Rewrite[J].Rewrite[0]))
	    fprintf(Output,"%s:%s\n",Rewrite[J].NewTag,Rewrite[J].Rewrite);
	 else
	    fprintf(Output,"%s: %s\n",Rewrite[J].NewTag,Rewrite[J].Rewrite);
      }      
   }
      
   return true;
}
APT_IGNORE_DEPRECATED_POP
									/*}}}*/

pkgTagSection::~pkgTagSection() { delete d; }
