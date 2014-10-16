#include <stdio.h>
#include <malloc.h>
#include <stdint.h>
#include <string.h>

static const unsigned char AccessUnitDelimiter[] = { 0x00, 0x00, 0x00, 0x01, 0x09, 0x10 };
static const unsigned char StartCode[] = {0x00, 0x00, 0x00, 0x01};

#define IOBUF_SIZE 4096

struct ParserCtx
{
	unsigned char* buf; 
	char iobuf[IOBUF_SIZE];
	size_t iobufpos,iobufsize;
	size_t typeStats[32];
	size_t naluLen, maxNaluSize, naluCount, maxNALUs, naluTotalBytes;
	int naluType,verbose,writeOutput,naluStartCode,zeros;
	unsigned char sc[4];
};

static inline size_t readInput(struct ParserCtx* ctx, unsigned char* buf, size_t n)
{
	size_t count=0;
	size_t p=0;

	size_t nCopyFromIOBuf = ctx->iobufsize - ctx->iobufpos;

	if( nCopyFromIOBuf == 0 )
	{
		ctx->iobufpos = 0;
		p = read(0,ctx->iobuf,IOBUF_SIZE);
		if( p < 0 ) return -1;
		ctx->iobufsize = p;
		nCopyFromIOBuf = ctx->iobufsize - ctx->iobufpos;
	}

	if( nCopyFromIOBuf > n ) nCopyFromIOBuf = n;
	memcpy(buf,ctx->iobuf+ctx->iobufpos,nCopyFromIOBuf);
	count += nCopyFromIOBuf;
	ctx->iobufpos += nCopyFromIOBuf;

	while(count<n)
	{
		p = read(0,buf+count,n-count);
		if( p < 0 ) { return -1; }
		if( p == 0 ) { return count; }
		count += p;
	}
	return count;
}


struct ParserCtx* allocContext()
{
	struct ParserCtx* ctx = (struct ParserCtx*)calloc(1,sizeof(struct ParserCtx));
	ctx->maxNALUs = -1;
	ctx->writeOutput = 1;
	ctx->naluStartCode = 1;
	ctx->maxNaluSize = 1<<20;
	ctx->buf = (unsigned char*) calloc(1,ctx->maxNaluSize);
	return ctx;
}

void freeContext(struct ParserCtx* ctx)
{
	if(ctx==0) return;
	if(ctx->buf!=NULL) free(ctx->buf);
	free(ctx); 
}

#define NALU_STATS(ctx) do { \
++ ctx->typeStats[ctx->naluType&31]; \
if( (ctx->verbose==1 && (ctx->naluCount%256)==0 ) || ctx->verbose>=2 ) \
{ \
    fprintf(stderr,"NALUs: %ld, parsed %ld Mb, last NALU: size=%ld, type=%d" \
           ,ctx->naluCount,ctx->naluTotalBytes/(1024*1024),ctx->naluLen,ctx->naluType); \
    if(ctx->verbose==1) fprintf(stderr,"              \r"); else if(ctx->verbose>=2) fprintf(stderr,"\n"); \
} \
}while(0)

#define NALU_FINAL_SATS(ctx) do {\
int _c; \
fprintf(stderr,"\nNALUs count: %ld, Total bytes %ld\n",ctx->naluCount,ctx->naluTotalBytes); \
for(_c=0;_c<32;_c++) { if(ctx->typeStats[_c]>0) fprintf(stderr,"NALU type %d count : %ld\n",_c,ctx->typeStats[_c]); } \
} while(0)

#define WRITE_NALU(ctx) do {\
  if( ctx->writeOutput && ctx->naluLen>0) { \
	if( ctx->naluStartCode ) { write(1,StartCode,4); } \
	else { write(1,ctx->sc,4); } \
	write(1,ctx->buf,ctx->naluLen); \
  } \
  NALU_STATS(ctx); \
} while(0)

#define CHECK_RESIZE_BUF(ctx) do {\
if( ctx->naluLen >= ctx->maxNaluSize ) { \
	ctx->maxNaluSize *= 2; \
	fprintf(stderr,"realloc NALU buffer to %ld bytes\n",ctx->maxNaluSize); \
	ctx->buf = (unsigned char*) realloc(ctx->buf,ctx->maxNaluSize); \
} \
} while(0)


int parseAnnexB(struct ParserCtx* ctx)
{
	while( ctx->naluCount<ctx->maxNALUs && readInput(ctx,ctx->buf+ctx->naluLen,1) == 1 )
	{
		++ctx->naluLen;
		if( ctx->buf[ctx->naluLen-1]==0x00 ) ++ ctx->zeros;
		else if( ctx->buf[ctx->naluLen-1]==0x01 && ctx->zeros==3 )
		{
			ctx->naluLen -= 4;
			ctx->sc[0] = (ctx->naluLen>>24) & 0xFF;
			ctx->sc[1] = (ctx->naluLen>>16) & 0xFF;
			ctx->sc[2] = (ctx->naluLen>>8) & 0xFF;
			ctx->sc[3] = ctx->naluLen & 0xFF;
			++ctx->naluCount;
			ctx->naluTotalBytes += ctx->naluLen;
			ctx->naluType = ctx->naluLen>0 ? (ctx->buf[0]&0x1F) : 0;
			WRITE_NALU(ctx);
			ctx->naluLen=0;
			ctx->zeros=0;
		}
		else { ctx->zeros=0; }
		CHECK_RESIZE_BUF(ctx);
	}
	WRITE_NALU(ctx);
	NALU_FINAL_SATS(ctx);
	return 1;
}

int parseMKVH264(struct ParserCtx* ctx)
{
	do 
	{
		ctx->naluLen = ( ((unsigned int)ctx->sc[0]) << 24 )
			| ( ((unsigned int)ctx->sc[1]) << 16 ) 
			| ( ((unsigned int)ctx->sc[2]) << 8 ) 
			| ((unsigned int)ctx->sc[3]) ;
		CHECK_RESIZE_BUF(ctx);

		size_t bytesRead = readInput(ctx,ctx->buf,ctx->naluLen);
		if( bytesRead < 0 )
		{
			fprintf(stderr,"I/O Error, aborting\n");
			return 0;
		}
		else if( bytesRead < ctx->naluLen )
		{
			fprintf(stderr,"Premature end of file, aborting\n");
			return 0;
		}
		++ ctx->naluCount;
		ctx->naluTotalBytes += ctx->naluLen;
		ctx->naluType = ctx->naluLen>0 ? (ctx->buf[0]&0x1F) : 0;
		WRITE_NALU(ctx);
	} while( ctx->naluCount<ctx->maxNALUs && readInput(ctx,ctx->sc,4) == 4 );

	NALU_FINAL_SATS(ctx);

	return 1;
}

int main(int argc, char* argv[])
{
	struct ParserCtx* ctx=NULL;
	int i,ok;
	ctx = allocContext();

	for(i=1;i<argc;i++)
	{
		if( strcmp(argv[i],"-mkv") == 0 ) ctx->naluStartCode = 0;
		else if( strcmp(argv[i],"-annexb") == 0 ) ctx->naluStartCode = 1;		
		else if( strcmp(argv[i],"-stat") == 0 ) ctx->writeOutput = 0;
		else if( strcmp(argv[i],"-nalucount") == 0 ) { ++i; ctx->maxNALUs = atoi(argv[i]); }
		else if( strcmp(argv[i],"-v") == 0 ) { ++ ctx->verbose; }
	}

	if( readInput(ctx,ctx->sc,4) != 4 ) return 1;
	if( ctx->sc[0]==0x00 && ctx->sc[1]==0x00 && ctx->sc[2]==0x00 && ctx->sc[3]==0x01 )
	{
		fprintf(stderr,"Reading Annex B input\n");
		ok = parseAnnexB(ctx);
	}
	else
	{
		fprintf(stderr,"Reading MKV's raw h264 format\n");
		ok = parseMKVH264(ctx);
	}

	freeContext(ctx);
	return (ok==0);
}

