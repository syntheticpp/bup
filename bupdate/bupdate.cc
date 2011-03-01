#include "httpget.h"
#include "fidx.h"
#include "wvcomstatus.h"
#include "wvbuf.h"
#include "wvstringlist.h"
#include "wvstrutils.h"
#include "wvdiriter.h"
#include <unistd.h>
#include <errno.h>


typedef void progfunc_t(const char *s);
typedef unsigned char  byte;

static void simple_print(const char *s)
{
    printf("%s", s);
    fflush(stdout);
}


// reassign this to change progress message printing function
progfunc_t *progfunc = simple_print;


void print(WvStringParm s)
{
    simple_print(s);
}


void print(WVSTRING_FORMAT_DECL)
{
    print(WvString(WVSTRING_FORMAT_CALL));
}


WvError _file_get(WvBuf &buf, WvStringParm filename,
		  int startbyte, int bytelen)
{
    WvComStatus err;
    
    FILE *f = fopen(filename, "rb");
    if (!f)
	return err.set(errno);
    
    fseek(f, 0, SEEK_END);
    ssize_t filesize = ftell(f);
    
    if (startbyte < 0)
	return err.set("startbyte must be >= 0");
    if (startbyte >= filesize)
	return err.set("startbyte(%s) must be <= filesize(%s)",
		       startbyte, filesize);
    if (!(bytelen == -1 || bytelen > 0))
	return err.set("bytelen(%s) must be -1 or >0", bytelen);
    if (bytelen>0 && startbyte+bytelen > filesize)
	return err.set("startbyte(%s)+bytelen(%s) >= filesize(%s)",
		       startbyte, bytelen, filesize);
    
    fseek(f, startbyte, SEEK_SET);
    if (bytelen < 0)
	bytelen = filesize - startbyte;
    byte *p = buf.alloc(bytelen);
    ssize_t len = fread(p, 1, bytelen, f);
    if (len != bytelen)
    {
	buf.unalloc(bytelen);
	err.set("read: expected %s bytes, got %s", bytelen, len);
    }
    fclose(f);
    return err;
}


WvError http_get(WvBuf &buf, WvStringParm url, int startbyte, int bytelen)
{
    //print("    getting %s (%s,%s)\n", url, startbyte, bytelen);
    if (url.startswith("file://"))
	return _file_get(buf, url+7, startbyte, bytelen);
    else
	return _http_get(buf, url, startbyte, bytelen);
}


WvString http_get_str(WvStringParm url)
{
    WvComStatus err;
    WvDynBuf b;
    err.set(http_get(b, url, 0, -1));
    if (err.isok())
	return b.getstr();
    else
	return WvString::null;
}


void http_get_to_file(WvStringParm filename, WvStringParm url)
{
    WvDynBuf b;
    WvComStatus err;
    err.set(http_get(b, url, 0, -1));
    if (!err.isok())
	return;
    size_t len = b.used();
    
    //print("Writing to: %s (%s bytes)\n", filename, b.used());
    FILE *f = fopen(filename, "wb");
    if (!f)
    {
	err.set(errno);
	return;
    }
    if (fwrite(b.get(len), 1, len, f) != len)
    {
	err.set(errno);
	return;
    }
    fclose(f);
}


bool is_url(WvStringParm s)
{
    return strstr(s, "://");
}


void targets_from_file(WvStringList &l, WvStringParm s)
{
    if (s.startswith("<"))
    {
	// it's HTML; pick out the anchors
	const char *cptr = s;
	while (cptr)
	{
	    cptr = strstr(cptr, "<a href=");
	    if (!cptr)
		break;
	    cptr += 8;
	    char quote = *cptr;
	    if (quote != '/' && quote != '\"')
		continue;
	    cptr++;
	    const char *eptr = strchr(cptr, quote);
	    if (!eptr)
		continue;
	    WvString ns;
	    size_t len = eptr-cptr;
	    ns.setsize(len+1);
	    char *optr = ns.edit();
	    strncpy(optr, cptr, len);
	    optr[len] = 0;
	    l.append(url_decode(ns));
	}
    }
    else
    {
	// it's not HTML; assume it's a one-per-line list of filenames
	l.split(s, "\n");
    }
}


WvString readfile_str(WvStringParm filename)
{
    byte buf[65536];
    WvDynBuf b;
    FILE *f = fopen(filename, "rb");
    if (!f)
	return WvString::null;
    size_t len;
    while ((len = fread(buf, 1, sizeof(buf), f)) >= 1)
	b.put(buf, len);
    fclose(f);
    return b.getstr();
}


bool exists(WvStringParm filename)
{
    struct stat st;
    return stat(filename, &st) == 0;
}


class Fidx;


struct FidxMapping
{
    Fidx *fidx;
    Sha sha;
    size_t ofs, size;
};


void eatsuffix(WvString &s, WvStringParm suffix)
{
    if (s.endswith(suffix))
    {
	char *cptr = s.edit();
	cptr[s.len() - suffix.len()] = 0;
    }
}


class Fidx
{
public:
    WvString name;
    WvDynBuf buf;
    const byte *bytes;
    WvError err;
    Sha filesha;
    size_t filesize;
    
    Fidx(WvStringParm _name) : name(_name)
    {
	WvComStatusIgnorer ig; // FIXME shouldn't be needed, but is
	err.set(_file_get(buf, name, 0, -1));
	bytes = NULL;
	eatsuffix(name, ".fidx");
	if (!exists(name))
	{
	    err.set_both(ENOENT, "%s does not exist", name);
	    return;
	}
	if (buf.used() < sizeof(FidxHdr))
	{
	    err.set(".fidx length < len(FidxHdr)"); 
	    return;
	}
	FidxHdr *h = (FidxHdr *)buf.get(sizeof(FidxHdr));
	assert(h);
	if (memcmp(h->marker, "FIDX", 4) != 0)
	{
	    err.set(".fidx has invalid FIDX header");
	    return;
	}
	uint32_t ver = ntohl(h->ver);
	if (ver != FIDX_VERSION)
	{
	    err.set(".fidx: got version %s, wanted %s", ver, FIDX_VERSION);
	    return;
	}
	
	bytes = buf.peek(0, buf.used());
	assert(bytes);
	
	// FIXME verify checksum before removing from buffer
	filesha = *(Sha *)(bytes + buf.used() - 20);
	buf.unalloc(20);
	
	int ln = len();
	filesize = 0;
	for (int e = 0; e < ln; e++)
	    filesize += ntohs(get(e)->size);
    }
    
    int len() const
    {
	return buf.used() / sizeof(FidxEntry);
    }
    
    FidxEntry *get(int elem) const
    {
	assert(elem >= 0);
	assert(elem < len());
	return (FidxEntry *)(bytes + elem*sizeof(FidxEntry));
    }
};

DeclareWvList(Fidx);


static int _fidx_mapping_compare(const void *_a, const void *_b)
{
    FidxMapping *a = (FidxMapping *)_a;
    FidxMapping *b = (FidxMapping *)_b;
    return memcmp(&a->sha, &b->sha, sizeof(a->sha));
}


static int _fidx_mapping_search(const void *_key, const void *_member)
{
    Sha *key = (Sha *)_key;
    FidxMapping *member = (FidxMapping *)_member;
    return memcmp(key, &member->sha, sizeof(member->sha));
}


class FidxMappings
{
public:
    FidxMapping *list;
    int count;
    
    FidxMappings(FidxList &l)
    {
	count = 0;
	FidxList::Iter i(l);
	for (i.rewind(); i.next(); )
	    count += i->len();
	
	list = new FidxMapping[count];
	
	int o = 0;
	for (i.rewind(); i.next(); )
	{
	    int len = i->len();
	    size_t ofs = 0;
	    for (int e = 0; e < len; e++)
	    {
		FidxEntry *ent = i->get(e);
		FidxMapping *m = list + (o++);
		memset(m, 0, sizeof(*m));
		m->fidx = i.ptr();
		m->sha = ent->sha;
		m->ofs = ofs;
		m->size = ntohs(ent->size);
		assert(m->ofs <= i->filesize);
		assert(m->ofs + ntohs(ent->size) <= i->filesize);
		ofs += ntohs(ent->size);
	    }
	}
	
	print("Mappings: %s total objects loaded.\n", count);
	qsort(list, count, sizeof(FidxMapping), _fidx_mapping_compare);
    };
    
    ~FidxMappings()
    {
	delete[] list;
    }
    
    FidxMapping *find(Sha &sha)
    {
	return (FidxMapping *)bsearch(&sha, list, count, sizeof(FidxMapping),
				      _fidx_mapping_search);
    }
};


int main(int argc, char **argv)
{
    if (argc != 2)
    {
	fprintf(stderr, "usage: %s <url>\n", argv[0]);
	return 1;
    }
    
    WvComStatus err;
    WvString baseurl(argv[1]);
    for (char *cptr = baseurl.edit(); cptr && *cptr; cptr++)
	if (*cptr == '\\')
	    *cptr = '/';
    
    WvStringList targets;
    if (baseurl.endswith(".fidx"))
    {
	// the baseurl is a particular fidx, not a file list, so just use
	// a file list of one.
	targets.append(getfilename(baseurl));
	baseurl = WvString("file://%s", baseurl);
    }
    else if (is_url(baseurl))
    {
	// it's an actual URL; download it
	print("Downloading base: %s\n", baseurl);
	WvString s = http_get_str(baseurl);
	// FIXME: what if we got http redirected?  We should save the new url
	targets_from_file(targets, s);
    }
    else
    {
	// not an URL, so it's a disk file
	struct stat st;
	while (baseurl.endswith("/"))
	    *strrchr(baseurl.edit(), '/') = 0;
	if (stat(baseurl, &st) != 0)
	    err.set(errno);
	else if (S_ISDIR(st.st_mode))
	{
	    // a directory
	    print("it's a dir\n");
	    WvDirIter di(baseurl);
	    for (di.rewind(); di.next(); )
		targets.append(di->name);
	    if (!baseurl.endswith("/"))
		baseurl.append("/");
	}
	else
	{
	    // an index file
	    print("it's a file\n");
	    targets_from_file(targets, readfile_str(baseurl));
	}
	baseurl = WvString("file://%s", baseurl);
    }
    
    if (!baseurl.endswith("/"))
	baseurl = getdirname(baseurl);
    while (baseurl.endswith("/"))
	*strrchr(baseurl.edit(), '/') = 0;
    
    {
	WvStringList::Iter i(targets);
	for (i.rewind(); i.next(); )
	{
	    *i = trim_string(i->edit());
	    if (!*i || i->startswith(".") || !i->endswith(".fidx"))
		i.xunlink();
	}
    }
    
    print("baseurl is: '%s'\n"
	  "Targets (%s):\n",
	  baseurl, targets.count());
    {
	WvStringList::Iter i(targets);
	for (i.rewind(); i.next(); )
	    print("  '%s'\n", *i);
    }
    
    if (targets.isempty())
	err.set("no target names found in baseurl");
    
    // load existing fidxes
    FidxList fidxes;
    WvStringList::Iter i(targets);
    for (i.rewind(); i.next(); )
    {
	Fidx *f = new Fidx(*i);
	if (f->err.isok())
	    fidxes.append(f, true);
	else
	    delete f;
    }
    FidxMappings mappings(fidxes);
    
    for (i.rewind(); i.next(); )
    {
	print("\n%s\n", *i);
	assert(!strchr(*i, '/'));
	assert(i->endswith(".fidx"));
	WvString fidxname = *i;
	WvString tmpname("%s.tmp", fidxname);
	WvString outname = fidxname;
	outname.edit()[outname.len()-5] = 0;  // remove .fidx
	WvString outtmpname("%s.tmp", outname);
	
	http_get_to_file(tmpname, WvString("%s/%s", baseurl, fidxname));
	
	Fidx fidx(tmpname), oldfidx(fidxname);

	if (oldfidx.err.isok() && fidx.err.isok() 
	    && fidx.filesha == oldfidx.filesha)
	{
	    print("    already up to date.\n");
	    unlink(tmpname);
	    unlink(outtmpname);
	    continue;
	}

	print("    changed! (old=%s, new=%s)\n",
	      oldfidx.err.isok(), fidx.err.isok());
	
	// predict the download
	int len = fidx.len();
	size_t missing = 0, chunks = 0;
	for (int e = 0; e < len; e++)
	{
	    FidxEntry *ent = fidx.get(e);
	    FidxMapping *m = mappings.find(ent->sha);
	    if (!m)
	    {
		missing += ntohs(ent->size);
		chunks++;
	    }
	}
	print("    need to download %s/%s bytes in %s chunks.\n",
	      missing, fidx.filesize, chunks);
	
	// do the download
	WvComStatus errx(fidx.name);
	FILE *outf = fopen(outtmpname, "wb");
	if (!outf)
	{
	    errx.set(errno);
	    continue;
	}
	size_t rofs = 0, got = 0;
	for (int e = 0; e < len; e++)
	{
	    // FIXME handle errors in here
	    // FIXME merge consecutive chunks together when possible
	    FidxEntry *ent = fidx.get(e);
	    size_t esz = ntohs(ent->size);
	    FidxMapping *m = mappings.find(ent->sha);
	    WvDynBuf b;
	    if (m)
	    {
		assert(m->size == esz);
		errx.set(_file_get(b, m->fidx->name, m->ofs, m->size));
	    }
	    else
	    {
		errx.set(http_get(b, WvString("%s/%s", baseurl, outname),
				  rofs, esz));
		got += esz;
		print("    %s/%s                  \r", got, missing);
	    }
	    if (b.used() == esz)
		fwrite(b.get(esz), 1, esz, outf);
	    else
	    {
		errx.set("problems reading data");
		break;
	    }
	    rofs += esz;
	}
	print("                                              \r");
	fclose(outf);
	
	// FIXME validate the final file checksum here for a sanity check
	if (errx.isok())
	{
	    unlink(fidxname);
	    unlink(outname);
	    rename(outtmpname, outname);
	    rename(tmpname, fidxname);
	}
    }
    
    if (!err.isok())
    {
	print("error was: %s\n", err.str());
	return 1;
    }
    return 0;
}
