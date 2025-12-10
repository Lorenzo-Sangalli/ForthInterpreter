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
	SYMBOL = 4,
	VAR_SET = 5
	//FLOAT = 6;
} ftype;

typedef struct fobj{
	int refcount;
	enum F_TYPE type;
	union{
		int i;
		struct{
			char *ptr;
			size_t len;
			int quoted:1; //true if it's a proper string or false if it's a symbol.
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

/*============================ FUNCTION TABLE DATA STRUCTURE ============================*/

/*Each entry is a symbol name associated with a function implementation*/
typedef struct FunctionTableEntry{	
	fobj *name; //(should be a symbol)
	void (*callback) (fcontext *ctx, fobj *name);
	fobj *user_func;
} funcentry;

typedef struct FunctionTable{
 	funcentry **tbl;
	size_t funCount;
} functable;
 
/*============================== VAR TABLE DATA STRUCTURE ===============================*/

typedef struct VarTableEntry{
	char *name;
	fobj *val;
} varentry;

typedef struct VarTable{
	varentry **tbl;
	size_t varCount;
} vartable;


/*============================== CONTESXT DATA STRUCTURE ==============================*/

/*Execution context*/
typedef struct fcontext{
	fobj *stack;
	functable functions;
	vartable variables;
}fcontext;

fobj *ctxGetFromTop(fcontext *ctx, int index){
	size_t stacklen = ctx->stack->list.len;
	assert(stacklen-1-index>=0);
	return ctx->stack->list.ele[stacklen-1-index];
}

/*================================= FORWARD DECLARATIONS ====================================*/
void listPop(fobj *l);
void release(fobj *o);
fobj *compile(char* prg);
void exec(fcontext *ctx, fobj *prg);
void *newContext();
void retain(fobj *o);
fobj * contextPop(fcontext *ctx);
/*================================= ALLOCATION WRAPPERS ====================================*/

/*Just a (more) safe malloc that checks for out of memory exceptions.*/
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

/*================================= OBJECT CONSTRUCTORS =================================*/

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

/*================================= FORTH FUNCTIONS HANDLING =================================*/

/* Push a new function in the context, it's up to the caller to set the callback*/

funcentry *newFunction(fcontext *ctx, fobj *name){
	size_t tbl_len = ctx->functions.funCount;
	ctx->functions.tbl = srealloc(ctx->functions.tbl, (tbl_len+1)*sizeof(ctx->functions.tbl[0]));
	
	ctx->functions.tbl[tbl_len] = smalloc(sizeof(funcentry));
	
	ctx->functions.tbl[tbl_len]->name = name;
	retain(name);
	ctx->functions.funCount++;
	ctx->functions.tbl[tbl_len]->callback = NULL;
	ctx->functions.tbl[tbl_len]->user_func = NULL;

	return ctx->functions.tbl[tbl_len];
}

funcentry *getFunction(fcontext *ctx, fobj *word){
	size_t nfun = ctx->functions.funCount;

	for (size_t i = 0; i < nfun; i++){
		if (strcmp(ctx->functions.tbl[i]->name->str.ptr ,word->str.ptr)==0){
			return 	ctx->functions.tbl[i];
		}
	}

	return NULL;
}

/*Register the 'callback' function in the 'functions' FunctionTable found in ctx.*/
void registerFunction(fcontext *ctx, char* symbol, 	void (*callback) (fcontext *ctx, fobj *name)){

	fobj *oname = newSymbolObject(symbol, strlen(symbol));
	funcentry *fe = getFunction(ctx,oname);
	if(fe!=NULL && fe->user_func!=NULL){
		release(fe->user_func);
		fe->user_func = NULL;
		fe->callback = callback;
	}
	else{
		fe = newFunction(ctx,oname);
		fe->callback = callback;
	}
	
	release(oname);

}

void registerUserFunction(fcontext *ctx, fobj *oname, fobj *expr){
	assert(expr->type == LIST && oname->type == SYMBOL);

	funcentry *fe = getFunction(ctx, oname);

	if (fe!=NULL && fe->callback!=NULL){
		fe->callback = NULL;
		fe->user_func = expr;
	}
	else{
		fe = newFunction(ctx,oname);
		fe->user_func = expr;
	}

	release(oname);
	
}
/*================================ VARIABLE HANDLING FUNCTIONS ================================*/

varentry *newVariable(fcontext *ctx, char *name){
	size_t tbl_len = ctx->variables.varCount;

	ctx->variables.tbl = srealloc(ctx->variables.tbl, (tbl_len+1)*sizeof(ctx->variables.tbl[0]));
	
	ctx->variables.tbl[tbl_len] = smalloc(sizeof(funcentry));
	ctx->variables.tbl[tbl_len]->name = smalloc(strlen(name)+1);
	strcpy(ctx->variables.tbl[tbl_len]->name, name);
	free(name);
	ctx->variables.varCount++;

	return ctx->variables.tbl[tbl_len];
}

varentry *getVariable(fcontext *ctx, char* name){
	size_t nvar = ctx->variables.varCount;
	for (size_t i = 0; i < nvar; i++){
		if (strcmp(ctx->variables.tbl[i]->name,name)==0){
			return ctx->variables.tbl[i];
		}
	}

	return NULL;
	
}

void registerVariable(fcontext *ctx, char *name, fobj*val){

	retain(val);
	varentry *ve = getVariable(ctx,name);

	if (ve!=NULL){
		release(ve->val);
	}
	else{
		ve = newVariable(ctx,name);
	}

	ve->val = val;
}

void registerVarSet(fcontext *ctx, fobj* varset){
	assert(varset->type==VAR_SET);

	size_t len = varset->list.len;
	for (size_t i = 0; i < len; i++)
	{
		assert(varset->list.ele[i]->type==SYMBOL);
		fobj *top = contextPop(ctx);
		registerVariable(ctx, varset->list.ele[i]->str.ptr, top);
		release(top);
	}
	
}

/*================================= OBJECT RELATED FUNCTIONS =================================*/

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
		free(o->list.ele);
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
	case BOOL:
		if(o->i) printf("true");
		else printf("false");
		break;
	case VAR_SET:
		printf("(");
		for(size_t i = 0; i<o->list.len; i++){
			fobj* ele = o->list.ele[i];
			echoObject(ele);
			if(i!=o->list.len-1){
				printf(" ");
			}
		}
		printf(")");
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
	size_t list_size= l->list.len;
	l->list.ele = srealloc(l->list.ele, sizeof(fobj*)*(list_size+1));
	l->list.ele[list_size]=ele;
	l->list.len++;
}

void listPop(fobj *l){
	size_t len = l->list.len;
	assert(len>0);
	release(l->list.ele[len-1]);
	l->list.len--;
}

/*================================= PARSING FUNCTIONS =================================*/
void parseSpaces(fparser *parser){
	while (parser->p && isspace(parser->p[0]))
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
	const char symchars[] = "+-*/%><=:;$";
	return (isalpha(c) || (strchr(symchars,c) && c!=0));
}

fobj *parseSymbol(fparser *parser){
	char *start = parser->p;

	while (isSymbolChar(parser->p[0])){
		parser->p++;
	}
	size_t symlen = parser->p - start;
	return newSymbolObject(start,symlen);
	
}

fobj *parseString(fparser *parser){
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

fobj *parseList(fparser *parser){
	parser->p++;
	char *start = parser->p;
	int countBrackets = 1;
	while (countBrackets>0 && parser->p[0]!=0)
	{	
		if(parser->p[0]==']'){
			countBrackets--;
		}
		if(parser->p[0]=='['){
			countBrackets++;
		}
		parser->p++;
	}
	if (parser->p[0]==0){
		return NULL;
	}
	
	size_t prglen = (parser->p) - start; 
	char *subprg = smalloc(prglen);
	memcpy(subprg,start,prglen);
	subprg[prglen-1] = 0;

	fobj* list = compile(subprg);
	free(subprg);

	parser->p++;
	return list;
}

fobj *parseVarSet(fparser *parser){
	parser->p++;
	fobj *varSet = newListObject();
	varSet->type = VAR_SET;
	while (parser->p[0] != ')')
	{
		parseSpaces(parser);

		if (!isalnum(parser->p[0])){
			fprintf(stderr,"ERROR: non alphanumeric characters not allowed in variable names\n");
			release(varSet);
			return NULL;
		}
		
		fobj *name = parseSymbol(parser);
		listPush(varSet,name);
	}
	parser->p++;
	return varSet;
}

fobj *compile(char* prg){
//	fprintf(stdout,"Compile started: %s\n",prg);
	fparser parser;
	parser.prg = prg;
	parser.p = prg;
	
	fobj *parsed = newListObject();

	while (parser.p)
	{
		fobj *o;
		char *token_start = parser.p;
		parseSpaces(&parser);

		if(parser.p[0]==0){ //End of program, reached null term.
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
		else if(parser.p[0]=='['){
			o = parseList(&parser);
		}
		else if(parser.p[0]=='('){
			o = parseVarSet(&parser);
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
			//retain(o);
		}
	}
	return parsed;
}

void lazyEval(fcontext *ctx){

	fobj *o = ctxGetFromTop(ctx,0);
	while (o->type==LIST)
	{	
		contextPop(ctx);
		exec(ctx,o);
		o=ctxGetFromTop(ctx,0);
	}	
}

/*================================= SYMBOL ASSOCIATED FUNCTIONS =================================*/


/*does basic math operations between integers such as: sums(+), subtractions(-), products(*), divisions(/), modulos(%).
 *in general all the basic math operations between two integers number that can be represented by one character should belong here.*/

void basicMathFunction(fcontext *ctx, fobj *name){
//	printf("BASIC MATH BEGIN\n");
	size_t stacklen = ctx->stack->list.len;
	assert(stacklen>=2);

	
	lazyEval(ctx);
	fobj *n1 = contextPop(ctx);
	
	lazyEval(ctx);
	fobj *n2 = contextPop(ctx);
	
	
	fobj *newO;
	if(n1->type!=INT || n2->type!=INT){
		printf("Error: unknown operand type\n");
		return;
	}

	assert(name->str.len==1);
	int res;
	int a = n1->i;
	int b = n2->i;
	switch (name->str.ptr[0])
	{
	case '+': res = a+b; break;
	case '-': res = a-b; break;
	case '*': res = a*b; break;
	case '/': res = a/b; break;
	case '%': res = a%b; break;
	default:
		printf("ERROR: unknown operation\n");
		exit(1);
		break;
	}

	newO = newIntObject(res);
	listPush(ctx->stack,newO);
}

/*basic function to make comparisons between two objects.
 *TODO: implement string comparison*/

void basicCompareFunction(fcontext *ctx, fobj *name){
	size_t stacklen = ctx->stack->list.len;
	assert(stacklen>=2);

	lazyEval(ctx);
	fobj *n1 = contextPop(ctx);

	lazyEval(ctx);
	fobj *n2 = contextPop(ctx);

	fobj *newO;
	assert(n1->type==INT && n2->type==INT);
	int res;
	int a = n1->i;
	int b = n2->i;
	switch (name->str.ptr[0])
	{
	case '>':
		res = (a>b);
		break;
	case '<':
		res = (a<b);
		break;
	case '=':
		res = (a==b);
		break;
	default:
		printf("ERROR: cannot compare\n");
		exit(1);
		break;
	}
	newO = newBoolObject(res);
	listPush(ctx->stack,newO);

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
		contextPop(ctx);
	}
	else if(strcmp(name->str.ptr,"swap")==0){
		assert(stacklen>=2);
		fobj *sec = ctx->stack->list.ele[stacklen-2];
		contextPop(ctx);
		contextPop(ctx);
		listPush(ctx->stack, top);
		listPush(ctx->stack, sec);
		retain(top);
		retain(sec);
	}
}

void basicCondFunction(fcontext *ctx, fobj *name){
	echoObject(ctx->stack);printf("\n");
	lazyEval(ctx);
	fobj *expr = ctxGetFromTop(ctx,0);
	contextPop(ctx);
	assert(expr->type == BOOL);

	int cond = expr->i;
	release(expr);
	
	if (strcmp(name->str.ptr,"if")==0){
		fobj *then_branch = ctxGetFromTop(ctx,0);
		contextPop(ctx);

		if(cond==1){
			exec(ctx, then_branch);
		}

		release(then_branch);
	}
	else if(strcmp(name->str.ptr,"ifelse")==0){
		fobj *then_branch = ctxGetFromTop(ctx,0);
		fobj *else_branch = ctxGetFromTop(ctx,1);
		contextPop(ctx);
		contextPop(ctx);

		assert(then_branch->type==LIST && else_branch->type==LIST);
		if(cond==1){
			exec(ctx,then_branch);
		}
		else if(cond==0){
			exec(ctx,else_branch);
		}

		release(then_branch);
		release(else_branch);
	}
}

void basicLoopFunction(fcontext *ctx, fobj *name){


	if(strcmp(name->str.ptr,"while")==0){
		fobj *expr = contextPop(ctx);
		fobj *body = contextPop(ctx);
		assert(expr->type==LIST && body->type==LIST);

		exec(ctx,expr);
		fobj *cond = contextPop(ctx);
		assert(cond->type==BOOL);
		
		while (cond->i)
		{
			exec(ctx,body);
			exec(ctx,expr);
			release(cond);
			cond = contextPop(ctx);
		}

		release(cond);
		release(expr);
		release(body);
	}
	
}

void declareFunction(fcontext *ctx, fobj *name){
	if(strcmp(name->str.ptr,";")==0){
		fobj *body = contextPop(ctx);
		fobj *symbol = contextPop(ctx);

		assert(symbol->type == SYMBOL && body->type == LIST);

		registerUserFunction(ctx,symbol,body);
	}
}

/*================================= EXEC AND CONTEXT =================================*/

void fillFunctionTable(fcontext *ctx){
	registerFunction(ctx,"+",basicMathFunction);
	registerFunction(ctx,"-",basicMathFunction);
	registerFunction(ctx,"*",basicMathFunction);
	registerFunction(ctx,"/",basicMathFunction);
	registerFunction(ctx,"%",basicMathFunction);
	registerFunction(ctx,"dup",basicStackFunction);
	registerFunction(ctx,"drop",basicStackFunction);
	registerFunction(ctx,"swap",basicStackFunction);
	registerFunction(ctx,">",basicCompareFunction);
	registerFunction(ctx,"<",basicCompareFunction);
	registerFunction(ctx,"=",basicCompareFunction);
	registerFunction(ctx,"if",basicCondFunction);
	registerFunction(ctx,"ifelse",basicCondFunction);
	registerFunction(ctx,"while",basicLoopFunction);
	registerFunction(ctx,";",declareFunction);
}

void *newContext(){
	fcontext *ctx = smalloc(sizeof(fcontext));
	ctx->stack = newListObject();
	
	ctx->functions.tbl = NULL;
	ctx->functions.funCount = 0;	

	ctx->variables.tbl = NULL;
	ctx->variables.varCount = 0;

	fillFunctionTable(ctx);

	return ctx; 
}

void freeContext(fcontext*ctx){
	release(ctx->stack);
	for(int i = 0; i<ctx->functions.funCount; i++){
		funcentry *fe = ctx->functions.tbl[i];

		release(fe->name);
		if (fe->user_func!=NULL) release(fe->user_func);

		free(fe);
	}

	free(ctx->functions.tbl);
	free(ctx);
}

void mergeContext(fcontext* ctx, fcontext *subctxt){
	for (size_t i = 0; i < subctxt->stack->list.len; i++)
	{
		listPush(ctx->stack, subctxt->stack->list.ele[i]);
		retain(subctxt->stack->list.ele[i]);
	}	
}

fobj* contextPop(fcontext* ctx){
	assert(ctx->stack->list.len>0);
	fobj* top = ctxGetFromTop(ctx,0);
	retain(top);
	listPop(ctx->stack);
	return top;
}

/*try to match the 'word' symbol with the proper function.
 *return 0 if a proper function was found, return 1 otherwise*/
int callSymbol(fcontext *ctx, fobj *word){
	size_t nfun = ctx->functions.funCount;
	
	funcentry *fe = getFunction(ctx,word);

	if(fe==NULL){
		int len = word->str.len;
		if(word->str.ptr[len-1]==':'){
			word->str.ptr[len-1]=0;
			listPush(ctx->stack,word);
			retain(word);
			return 0;
		}
		else if(word->str.ptr[0] == '$'){
			word->str.ptr++;
			varentry *ve = getVariable(ctx,word->str.ptr);
			if (ve==NULL){
				return 1;
			}
			listPush(ctx->stack,ve->val);
			retain(word);
			return 0;
		}
	}

	if(fe->callback!=NULL)fe->callback(ctx,word);
	else exec(ctx, fe->user_func);

	return 0;
}
/*Execute the program stored in the prg list*/
void exec(fcontext *ctx, fobj *prg){
	
	assert(prg->type==LIST);
	for (size_t i = 0; i < prg->list.len; i++)
	{
		fobj *word = prg->list.ele[i];
		switch (word->type)
		{
		case SYMBOL:
			if(callSymbol(ctx,word)) printf("error: cannot resolve %s symbol\n", word->str.ptr);
			break;
		case VAR_SET:
			registerVarSet(ctx,word);
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
	prgtext[file_size] = 0;
	printf("text: %s\n",prgtext);
	fclose(fp);

	
	fobj *prg = compile(prgtext);

	fcontext *ctx = newContext();
	exec(ctx, prg);

	printf("Stack: ");
	echoObject(ctx->stack);
	printf("\n");

	freeContext(ctx);
	release(prg);
	free(prgtext);

	return 0;
}
