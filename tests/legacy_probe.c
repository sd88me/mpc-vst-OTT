#include <stdio.h>
#include <stdint.h>
#include <dlfcn.h>
typedef struct AEffect AEffect;
typedef intptr_t (*Disp)(AEffect*,int32_t,int32_t,intptr_t,void*,float);
struct AEffect{int32_t magic;Disp dispatcher;void*p;void(*setParameter)(AEffect*,int32_t,float);float(*getParameter)(AEffect*,int32_t);int32_t numPrograms,numParams,numInputs,numOutputs,flags;};
static intptr_t host(AEffect*e,int32_t op,int32_t i,intptr_t v,void*p,float o){return op==1?2400:0;}
int main(int c,char**v){void*h=dlopen(v[1],RTLD_NOW);if(!h){puts(dlerror());return 1;}
AEffect*(*m)(void*)=dlsym(h,"VSTPluginMain");AEffect*e=m(host);
printf("params=%d programs=%d flags=%#x\n",e->numParams,e->numPrograms,e->flags);
e->dispatcher(e,0,0,0,0,0);e->setParameter(e,0,0.25f);
void*ptr=0;intptr_t n=e->dispatcher(e,23,0,0,&ptr,0);printf("effGetChunk bank -> %ld bytes\n",(long)n);
n=e->dispatcher(e,23,1,0,&ptr,0);printf("effGetChunk program -> %ld bytes\n",(long)n);return 0;}
