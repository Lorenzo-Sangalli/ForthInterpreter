#include<stdio.h>
#include<unistd.h>
#include<stdlib.h>
#include<ctype.h>
#include<string.h>
#include<assert.h>



/*================================= DATA STUCTURES  =================================*/

typedef struct fcontext fcontext;

typedef enum F_TYPE{
	INT = 0,
	STR = 1,
	BOOL = 2,
	LIST = 3,
	SYMBOL = 4
	//FLOAT = 5;
} ftype;

typedef struct fobj{
	int refcount;
	enum F_TYPE type;
	union{
		int i;
		struct{
			char *ptr;
			size_t len;
			int quoted:1; //true or false;
		}str;
		struct list {
			struct fobj **ele;
			size_t len; //number of elements in the list
			size_t size; //space allocated for the list. TODO: double size every time needed, for O(1) ammortized insert operations  
		}list;
	};
} fobj;

typedef struct fparser{
	char *prg; //prgoram to compile.
	char *p; //next token to parse.
}fparser;

/*Each entry is a symbol name associated with a function implementation*/
struct FunctionTableEntry{	
	fobj *name; //(should be a symbol)
	void (*callback) (fcontext *ctx, fobj *name);
	fobj *user_list;
};

struct FunctionTable{
	struct FunctionTableEntry **tbl;
	size_t funCount;
};

/*Execution context*/
typedef struct fcontext{
	fobj *stack;
	struct FunctionTable functions;
}fcontext;

/*================================= FORWARD DECLARATIONS ========================================*/
void listPop(fobj *l);
void release(fobj *o);
/*================================= ALLOCATION WRAPPERS =========================================*/

void *smalloc(size_t size){
	void *ptr = malloc(size);
	if(ptr == NULL){
		fprintf(stderr,"Error: Out of memory, tried to allocate %zu bytes\n",size);
		exit(1);
	}

	return ptr;
}

void *srealloc(void *oldptr, size_t size){
	void *ptr = realloc(oldptr,size);
	if(ptr==NULL){
		fprintf(stderr,"Out of memory\n");
		exit(1);
	}
	return ptr;
}

/*================================= OBJECT RELATED FUNCTIONS =================================*/

/*Allocate and initizialize a new object of the desired ftype*/

fobj *newObject(ftype type){
	fobj *o = (fobj*)smalloc(sizeof(fobj));
	o->type = type;
	o->refcount = 1;
	
	return o;
}

fobj *newStringObject(char *s, size_t len){
	fobj *o = newObject(STR);
	o->str.ptr = smalloc(len+1);
	o->str.len = len;
	memcpy(o->str.ptr, s, len);
	o->str.ptr[len] = 0;
	return o;
}

fobj *newSymbolObject(char *s, size_t len){
	fobj *o = newStringObject(s,len);
	o->type=SYMBOL;
	
	return o;
}

fobj *newIntObject(int i){
	fobj *o = newObject(INT);
	o->i=i;
	return o;
}

fobj *newBoolObject(int i){
	fobj *o = newObject(BOOL);
	o->i=i;
	return o;
}

fobj *newListObject(void){
	fobj *o = newObject(LIST);
	o->list.ele = NULL;
	o->list.len = 0;

	return o;
}


/*Register the 'callback' function in the 'functions' FunctionTable found in ctx.*/
void registerFunction(fcontext *ctx, char* symbol, 	void (*callback) (fcontext *ctx, fobj *name)){
	size_t tbl_len = ctx->functions.funCount;

	ctx->functions.tbl = srealloc(ctx->functions.tbl, (tbl_len+1)*sizeof(ctx->functions.tbl[0]));
	
	ctx->functions.tbl[tbl_len] = smalloc(sizeof(struct FunctionTableEntry));
	ctx->functions.tbl[tbl_len]->callback = callback;
	
	fobj *symobj = newSymbolObject(symbol,strlen(symbol));
	ctx->functions.tbl[tbl_len]->name = symobj;

	ctx->functions.funCount++;
}
void retain(fobj *o){
	o->refcount++;

}	

/*Free an object even if it has a complex or nested type e.g: list or str*/
void freeObject(fobj *o){
	switch (o->type)
	{
	case LIST:
		for(size_t i = 0; i<o->list.len; i++){
			fobj* ele = o->list.ele[i];
			release(ele);
		}
		break;
	case SYMBOL:
	case STR:
		free(o->str.ptr);
		break;
	default:
		break;
	}

	free(o);
}


void release(fobj *o){
	assert(o->refcount>0);
	o->refcount--;
	if(o->refcount==0){
		freeObject(o);
	}
}

void echoObject(fobj *o){
	switch (o->type)
	{
	case INT:
		printf("%d", o->i);
		break;
	case LIST:
		printf("[");
		for(size_t i = 0; i<o->list.len; i++){
			fobj* ele = o->list.ele[i];
			echoObject(ele);
			if(i!=o->list.len-1){
				printf(" ");
			}
		}
		printf("]");
		break;
	case SYMBOL:
		printf("%s", o->str.ptr);
		break;
	case STR:
		printf("\"%s\"",o->str.ptr);
		break;
	default:
		printf("unknown type");
		break;
	}
}

/*================================= LIST OBJECT =================================*/

/*Add a new element at the end of the list
 *It is up to the caller to increment the reference count
 *of the added element if needed. */
void listPush(fobj *l, fobj * ele){
//	printf("LIST PUSH: ");
//	echoObject(ele);
	size_t list_size= l->list.len;
	l->list.ele = srealloc(l->list.ele, sizeof(fobj*)*(list_size+1));
	l->list.ele[list_size]=ele;
	l->list.len++;
//	printf("\n SUCCESS \n");
	
}

void listPop(fobj *l){
//	printf("RELEASE: ");
//	echoObject(l);
	size_t len = l->list.len;
	release(l->list.ele[len-1]);
	l->list.len--;
//	printf("\nSUCCESS\n");
}

/*================================= MAKE PROGRAM INTO A LIST =================================*/
void parseSpaces(fparser *parser){
	while (parser->p[0]!=0 && isspace(parser->p[0]))
	{
		parser->p++;
	}
}

#define MAX_NUM_LEN 128
fobj *parseNumber(fparser *parser){
	char buffer[MAX_NUM_LEN+1];
	int i = 0;
	int isNegative = 1;
	if(parser->p[0] == '-'){
		parser->p++;
		isNegative=-1;
	}
	while (isdigit(parser->p[0]) && parser->p[0]!='\0'){
		if(i>=128){
			return NULL;
		}
		buffer[i]=parser->p[0];
		parser->p++;
		i++;
	}

	buffer[i]=0;
	int n = atoi(buffer);
	n*=isNegative;
	fobj *o = newIntObject(n);
	return o;
}

int isSymbolChar(char c){
	const char symchars[] = "+-*/%";
	return (isalpha(c) || strchr(symchars,c));
}

fobj *parseSymbol(fparser *parser){
	char *start = parser->p;
	while (isSymbolChar(parser->p[0])){
		parser->p++;
	}
	size_t symlen = parser->p - start;
	return newSymbolObject(start,symlen);
	
}

fobj* parseString(fparser *parser){
	parser->p++;
	char *start = parser->p;
	while (parser->p[0]!='"')
	{
		parser->p++;
	}
	size_t strlen = (parser->p) - start;
	parser->p++;
	return newStringObject(start,strlen);
}

fobj *compile(char* prg){
//	fprintf(stderr,"Compile started\n");
	fparser parser;
	parser.prg = prg;
	parser.p = prg;
	
	fobj *parsed = newListObject();

	while (parser.p)
	{
		fobj *o;
		char *token_start = parser.p;
		parseSpaces(&parser);
		if(parser.p[0]==0){ //End of program, reached null term
			break; 
		}
		if(isdigit(parser.p[0]) || (parser.p[0]=='-' && isdigit(parser.p[1]))){
			o = parseNumber(&parser);
		}
		else if(parser.p[0]=='"'){
			o = parseString(&parser);
		}
		else if(isSymbolChar(parser.p[0])){
			o = parseSymbol(&parser);
		}
		else{
			o = NULL;
		}

		if(o==NULL){
			release(parsed);
			fprintf(stderr,"Syntax error near: %32s .. \n",token_start);
			return NULL;
		}
		else {
			listPush(parsed,o);
		}
	}
	return parsed;
}

/*================================= SYMBOL ASSOCIATED FUNCTIONS =================================*/


/*does basic math operations between integers such as: sums(+), subtractions(-), products(*), divisions(/), modulos(%).
 *in general all the basic math operations between two integers number that can be represented by one character should belong here.*/

void basicMathFunction(fcontext *ctx, fobj *name){
//	printf("BASIC MATH BEGIN\n");
	size_t stacklen = ctx->stack->list.len;
	assert(stacklen>=2);

	fobj *n1 = ctx->stack->list.ele[stacklen-1];
	fobj *n2 = ctx->stack->list.ele[stacklen-2];
	listPop(ctx->stack);
	listPop(ctx->stack);
	fobj *newO;
	if(n1->type!=INT && n2->type!=INT){
		fprintf(stderr, "Error: unknown operand type\n");
	}

	assert(name->str.len==1);
	int res;
	switch (name->str.ptr[0])
	{
	case '+':
		res = n1->i+n2->i;
		break;
	case '-':
		res = n2->i-n1->i; 
		break;
	case '*':
		res = (n1->i)*(n2->i);
		break;
	case '/':
		res = (n1->i)/(n2->i);
		break;
	case '%':
		res = (n1->i)%(n2->i);
		break;
	default:
		printf("ERROR: unknown operation\n");
		exit(1);
		break;
	}

	newO = newIntObject(res);
	listPush(ctx->stack,newO);
	retain(newO);
}

void basicStackFunction(fcontext *ctx, fobj *name){
	size_t stacklen = ctx->stack->list.len;
	assert(stacklen>=1);
	fobj *top = ctx->stack->list.ele[stacklen-1];
	if(strcmp(name->str.ptr,"dup")==0){
		listPush(ctx->stack,top);
		retain(top);
	}
	else if(strcmp(name->str.ptr,"drop")==0){
		listPop(ctx->stack);
	}
	else if(strcmp(name->str.ptr,"swap")==0){
		assert(stacklen>=2);
		fobj *sec = ctx->stack->list.ele[stacklen-2];
		listPop(ctx->stack);
		listPop(ctx->stack);
		listPush(ctx->stack, top);
		listPush(ctx->stack, sec);
		retain(top);
		retain(sec);
	}
}

/*================================= EXEC AND CONTEST =================================*/

void fillFunctionTable(fcontext *ctx){
	registerFunction(ctx,"+",basicMathFunction);
	registerFunction(ctx,"-",basicMathFunction);
	registerFunction(ctx,"*",basicMathFunction);
	registerFunction(ctx,"/",basicMathFunction);
	registerFunction(ctx,"%",basicMathFunction);
	registerFunction(ctx,"dup",basicStackFunction);
	registerFunction(ctx,"drop",basicStackFunction);
	registerFunction(ctx,"swap",basicStackFunction);
}

void *newContext(){
	fcontext *ctx = smalloc(sizeof(fcontext));
	ctx->stack = newListObject();
	ctx->functions.tbl = NULL;
	ctx->functions.funCount = 0;	

	fillFunctionTable(ctx);

	return ctx; 
}

/*try to match the 'word' symbol with the proper function.
 *return 0 if a proper function was found, return 1 otherwise*/
int callSymbol(fcontext *ctx, fobj *word){
//	printf("CALL SYMBOL BEGIN\n");
	size_t nfun = ctx->functions.funCount;
	int found = 0;
//	printf("word: %s\n",word->str.ptr);

	for (size_t i = 0; i < nfun; i++)
	{
//		printf("fun: %s\n",ctx->functions.tbl[i]->name->str.ptr);
		if (strcmp(ctx->functions.tbl[i]->name->str.ptr ,word->str.ptr)==0)
		{
			found = 1;
			ctx->functions.tbl[i]->callback(ctx,word);
			break;
		}
	}

	return found;
}
/*Execute the program stored in the prg list*/
void exec(fcontext *ctx, fobj *prg){
//	printf("EXEC BEGIN\n");
	assert(prg->type==LIST);
	for (size_t i = 0; i < prg->list.len; i++)
	{
		fobj *word = prg->list.ele[i];
		switch (word->type)
		{
		case SYMBOL:
			callSymbol(ctx,word);
			break;
		default:
			listPush(ctx->stack, word);
			retain(word);
			break;
		}
	}
	
}

/*================================= MAIN =================================*/
	
int main(int argc, char **argv){
	
	if(argc!=2){
		fprintf(stderr,"Usage: %s <filename>\n",argv[0]);
		return 1;
	}

	FILE *fp = fopen(argv[1],"r");
	if(fp == NULL){
		perror("Failed to open the program\n");
		return 1;
	}

	fseek(fp,0,SEEK_END);
	long file_size = ftell(fp);
	char *prgtext = smalloc(file_size+1);
	fseek(fp,0,SEEK_SET);
	fread(prgtext,file_size,1,fp);
	prgtext[file_size] = '\0';
	printf("text: %s",prgtext);
	fclose(fp);

	
	fobj *prg = compile(prgtext);
	if(prg!=NULL){
		echoObject(prg);
		printf("\n");
	}

	fcontext *ctx = newContext();
	exec(ctx, prg);

	printf("Stack: ");
	echoObject(ctx->stack);
	printf("\n");
	return 0;
}
