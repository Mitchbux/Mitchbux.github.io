#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <windows.h>
#include <compressapi.h>

/* -------- aux stuff ---------- */

typedef uint8_t byte;

void *mem_alloc(size_t item_size, size_t n_item) {
	size_t *x = (size_t *)calloc(1, sizeof(size_t) * 2 + n_item * item_size);
	x[0] = item_size;
	x[1] = n_item;
	return x + 2;
}

void *mem_extend(void *m, size_t new_n) {
	size_t *x = (size_t *)m - 2;
	x = (size_t *)realloc(x, sizeof(size_t) * 2 + *x * new_n);
	if (new_n > x[1])
		memset((char *)(x + 2) + x[0] * x[1], 0, x[0] * (new_n - x[1]));
	x[1] = new_n;
	return x + 2;
}

void _clear(void *m) {
	size_t *x = (size_t *)m - 2;
	memset(m, 0, x[0] * x[1]);
}

#define _new(type, n) mem_alloc(sizeof(type), n)
#define _del(m) free((size_t *)(m)-2);
#define _len(m) *((size_t *)m - 1)
#define _setsize(m, n) m = (byte *)mem_extend(m, n)
#define _extend(m) m = (byte *)mem_extend(m, _len(m) * 2);

/*--------- bit reader/writer---------*/

typedef struct {
	int power;
	int bits;
	int start;
	char *data;
} writer_t;

void write_bit(writer_t *p, int v) {
	p->bits += (v & 1) << (8 - (++p->power));
	if (p->power >= 8) {
		p->data[p->start++] = p->bits;
		p->power = 0;
		p->bits = 0;
	}
}

void write_value(writer_t *p, unsigned long v, int n) {
	while (n > 0) {
		n--;
		write_bit(p, (v >> n) & 1);
	}
}

void flushWrite(writer_t *p) {
	while (p->power > 0)
		write_bit(p, 0);
}

typedef struct {
	int power;
	int bits;
	int start;
	char *data;
} reader_t;

int read_bit(reader_t *r) {
	int v = (r->bits >> (8 - (++r->power)) & 1);
	if (r->power >= 8) {
		r->power = 0;
		r->bits = r->data[r->start++];
	}
	return v;
}

unsigned int read_value(reader_t *r, int n) {
	unsigned int v = 0;
	while (n > 0) {
		n--;
		v = v << 1;
		v += read_bit(r);
	}
	return v;
}

writer_t *new_writer(byte *out) {
	writer_t *r = _new(writer_t, 1);
	r->power = 0;
	r->bits = 0;
	r->start = 0;
	r->data = (char *)out;
	return r;
}

reader_t *new_reader(byte *in) {
	reader_t *r = _new(reader_t, 1);
	r->power = 0;
	r->bits = in[0];
	r->start = 1;
	r->data = (char *)in;
	return r;
}

writer_t *flash(writer_t* w) {
	writer_t *r = _new(writer_t, 1);
	r->power = w->power;
	r->bits = w->bits;
	r->start = w->start;
	r->data = w->data;
	return r;
}

/*----- sx related -----*/

int lf(unsigned long n) {
	int m = 1;
	while ((unsigned long)(1 << m) < n)
		m++;
	return m;
}

#define sx_chunk_bits 28
#define sx_chunk 1 << sx_chunk_bits

#define sx_bits 6
#define sx_values 64
#define sx_items 3
#define sx_top 1000

void merge (writer_t *w, int *a, int n, int m) {
    int i, j, k;
    int *x = malloc(n * sizeof (int));
    for (i = 0, j = m, k = 0; k < n; k++) {
		if(j<n&&i<m&&w)write_bit(w, a[j]<a[i]?1:0);
        x[k] = j == n      ? a[i++]
             : i == m      ? a[j++]
             : a[j] < a[i] ? a[j++]
             :               a[i++];
    }
    for (i = 0; i < n; i++) {
        a[i] = x[i];
    }
    free(x);
}

void merge_sort (writer_t *w, int *a, int n) {
    if (n < 2)
        return;
    int m = n / 2;
    merge_sort(w, a, m);
    merge_sort(w, a + m, n - m);
    merge(w, a, n, m);
}

void merge_read (reader_t *r, int *a, int n, int m) {
    int i, j, k;
    int *x = malloc(n * sizeof (int));
    for (i = 0, j = m, k = 0; k < n; k++) {
        x[k] = j == n      ? a[i++]
             : i == m      ? a[j++]
             : read_bit(r) ? a[j++]
             :               a[i++];
    }
    for (i = 0; i < n; i++) {
        a[i] = x[i];
    }
    free(x);
}

void merge_sort_read (reader_t *r, int *a, int n) {
    if (n < 2)
        return;
    int m = n / 2;
    merge_sort_read(r, a, m);
    merge_sort_read(r, a + m, n - m);
    merge_read(r, a, n, m);
}


typedef struct {
	void *data[sx_values];
	int cnt;
} zsx_node;

void zsx_set(zsx_node *root, int *x, int len, int data) {
	zsx_node *destination = (zsx_node *)(root->data[x[sx_items - len]]);
	if (destination == 0) {
		destination = _new(zsx_node, 1);
		for (int i = 0; i < sx_values; i++)
			destination->data[i] = 0;
		root->data[x[sx_items - len]] = destination;
	}
	if (len == 1) {
		destination->cnt = data;
	}
	else {
		zsx_set(destination, x, len - 1, data);
	}
}

int zsx_get(zsx_node *root, int *x, int len) {
	zsx_node *destination = (zsx_node *)(root->data[x[sx_items - len]]);
	if (destination == 0)
		return 0;
	if (len == 1)
		return destination->cnt;
	else
		return zsx_get(destination, x, len - 1);
}

void zsx_del(zsx_node *root) {
	for (int i = 0; i < sx_values; i++)
		if (root->data[i])
			zsx_del(root->data[i]);
	_del(root)
}

char *bytes_buffer;
char *result_buffer;

int cx[sx_values][sx_values][sx_values];
int cs[sx_values][sx_values];

int play[sx_values][1<<19];
int next[256*1024];
int work[sx_values][sx_top];
int hard[1<<19];
int done[sx_values];

#define ZSX


/****** zsx ******/
byte *zsx_encode(byte *data, int len) {
	int z, s, x, k, v, n;
	byte *result;
	writer_t *bytes = new_writer(bytes_buffer);
	reader_t *reader = new_reader(data);
	write_value(bytes, len, 32);

	printf(".");

	zsx_node *root = _new(zsx_node,1);
	

	int last = 0;
	int count = 0;
		
	int *list = _new(int, 4);
	
	int task = 0;
	
	int max = 0;

	int dx[sx_values][sx_values];
	int wx[sx_values];
	for(s=0;s<sx_values;s++)
	{
		wx[s]=0;
		
		for(x=0;x<sx_values;x++)
		dx[s][x]=0;
	}
	
	for(s=0;s<sx_values;s++)
	{
		done[s]=0;
		for(x=0;x<(1<<19);x++)
			play[s][x]=0,hard[x]=0;
		
		for(x=0;x<sx_top;x++)
			work[s][x]=0;
		
		for(x=0;x<sx_values;x++)
		{
			cs[s][x]=0;
			for(v=0;v<sx_values;v++)
				cx[s][x][v]=0;
		}
	}
	while (reader->start <= len) {
		
		for (s = 0; s < sx_items; s++) {
			list[s] = read_value(reader, sx_bits);
		}
		
		int index = zsx_get(root, list, sx_items);
		write_bit(bytes, index?1:0);
		if (index)
		{
			write_value(bytes, index-1, lf(last));
		}else
		{
			int nx= (max)?next[--max]:++last;
			zsx_set(root, list, sx_items, nx);
			z=list[0];
			hard[nx*2] = list[1];
			hard[nx*2+1] = list[2];
			play[z][nx]=done[z];
			work[z][done[z]++]=nx;
			
			write_value(bytes, z, sx_bits);
			cs[z][list[1]]=1;
			cx[z][list[1]][list[2]]=1;
			
			if(done[z]==sx_top)
			{		
	
				for(x=0;x<sx_values;x++)
				{
					write_bit(bytes, cs[z][x]);
					if(cs[z][x])
					for(v=0;v<sx_values;v++)
						write_bit(bytes, cx[z][x][v]);
				}

				zsx_node *node = root->data[z];
				for(s=0;s<sx_values;s++)if(cs[z][s])
				for(x=0;x<sx_values;x++)if(cx[z][s][x])
				{
					zsx_node *rest = node->data[s];
					if (rest) rest = rest->data[x];
					if (rest)
					if (rest->cnt) write_value(bytes, play[z][rest->cnt], lf(done[z]));
				}
	
				for(s=0;s<done[z]*2;)
				{
					int idx = work[z][s];
					list[1] = hard[idx*2];
					list[2] = hard[idx*2+1];
					next[max++] = idx;
					zsx_set(root, list, sx_items, 0);
					work[z][s] = 0;
					hard[idx*2] = 0;
					hard[idx*2+1] = 0;
					play[z][idx]=0;
				}
				
				
				
				done[z]=0;
				for(x=0;x<sx_values;x++)
				{
					cs[z][x]=0;
					for(v=0;v<sx_values;v++)
						cx[z][x][v]=0;
				}
		
			}	
			
			
		}
		
	}

		for(s=0;s<last;s++)if(hard[s*2])
		{
			write_value(bytes, hard[s*2], sx_bits);
			write_value(bytes, hard[s*2+1], sx_bits);
		}
		
		zsx_del(root);
		
		//printf("%d ", bytes->start);
	flushWrite(bytes);
	*((size_t *)result_buffer - 1) = bytes->start;
	result = result_buffer;

	memcpy(result, bytes->data, bytes->start);

	return result;
}

byte *zsx_decode(byte *data, int len) {
	reader_t *bytes = new_reader(data);
	unsigned int z, s, x, size = read_value(bytes, 32);

	puts("decoding...");

	byte *result = _new(byte, size+12);
	*((size_t *)result_buffer - 1) = size;
	writer_t *w = new_writer(result);
	
	int **dico = _new(int *, sx_chunk);
	int **codi = _new(int *, sx_chunk);
	int *codes = _new(int, len);
	int items = size;
	int decoded = 0;
	int count = 0;
	int index = 0;
	int last = 0;
	for(s=0;s<sx_chunk;s++)
	{
		dico[s] = _new(int, 4);
		codi[s] = _new(int, 4);
	}
	int bitsSaved = 0;
	while (decoded < items) {
		bitsSaved+=7;
		if(read_bit(bytes))
		{
			index = read_value(bytes, lf(last));
			bitsSaved -= lf(last);
			codes[decoded++] = index;
		}else
		{
			codes[decoded++]=last++;
		}
		
	}
	
		
	//TODO

	
	for(s=0;s<sx_chunk;s++)
	{
		_del(codi[s]);
		_del(dico[s]);
	}
	_del(codi);
	_del(dico);
	_del(codes);
	
	return result;
}


void BackLine() {
	CONSOLE_SCREEN_BUFFER_INFO *info = _new(CONSOLE_SCREEN_BUFFER_INFO, 1);
	GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), info);
	COORD XY;
	XY.X = 0;
	XY.Y = info->dwCursorPosition.Y;
	SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), XY);
}

int bytes_read;

void CALLBACK FIO(DWORD E1, DWORD N2, LPOVERLAPPED ol) {
	bytes_read = N2;
}

int test() {
	
	OVERLAPPED ol;
	ol.Offset  = 0;
	ol.OffsetHigh = 0;

	HANDLE hFile = CreateFile(
		"enwik9",
		GENERIC_READ,
		FILE_SHARE_READ,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
		NULL);

	if (hFile == INVALID_HANDLE_VALUE) {
		printf("Can't read file!\n");
		return 0;
	};

	int encoded = 0;
	DWORD file_high;
	int file_size = GetFileSize(hFile, &file_high);

	bytes_buffer = _new(char, sx_chunk);
	result_buffer = _new(char, sx_chunk);

	FILE *f = fopen("enwik9.zsx", "wb");

	if (f == NULL) {
		printf("Can't create file!\n");
		return 0;
	};

	printf("encoding...\n");

	
	while (encoded < file_size) {
		ReadFileEx(hFile, bytes_buffer, sx_chunk, &ol, FIO);
		SleepEx(12000, TRUE);
		encoded += bytes_read;
		ol.Offset += bytes_read;
		int prev_len = bytes_read;
		
	#ifdef ZSX	
		COMPRESSOR_HANDLE Compressor = NULL;
		CreateCompressor(3, NULL, &Compressor);

		

		size_t csize;
		Compress(Compressor, bytes_buffer, bytes_read, NULL, 0, &csize);
		Compress(Compressor, bytes_buffer, bytes_read, result_buffer, csize, &csize);
		
		byte *data = zsx_encode(result_buffer, csize);
		size_t len_zsx = _len(data);
		
		
		while(len_zsx < prev_len)
		{
		prev_len = len_zsx;
		
		Compress(Compressor, bytes_buffer, len_zsx, NULL, 0, &csize);
		Compress(Compressor, bytes_buffer, len_zsx, result_buffer, csize, &csize);

		data = zsx_encode(result_buffer, csize);
		len_zsx = _len(data);

		}
				
		printf("final size: %ld\n", len_zsx);
		fwrite(&len_zsx, sizeof(size_t), 1, f);
		fwrite(data, len_zsx, 1, f);
		BackLine();
		printf("%.2f%%", (double)encoded / (double)file_size * 100.0);
	#else
		memcpy(result_buffer, bytes_buffer, bytes_read);
		data = zsx_encode(result_buffer, bytes_read);
		len_zsx = _len(data);
		
		byte *decompressed = zsx_decode(data, len_zsx);
		puts(decompressed);
		fwrite(decompressed, _len(decompressed), 1, f);
		BackLine();
		printf("%.2f%%", (double)encoded / (double)file_size * 100.0);		
		
	#endif
	}
	
	fclose(f);
	CloseHandle(hFile);

	return 1;
}


int main(int argc, char **argv) {
	test();
	return 0;
}
