#include <runtime.h>
#include <string-map.h>
#include <string.h>
#include <fnmatch.h>

struct string_string_map {
	char **pattern;
	char **value;
};

string_map_t SSMcreateSWP(const char* swp_spec){
	string_map_t pol=RT_NEW(struct string_string_map);
	int N=1;
	char*pattern=strdup(swp_spec);
	char*tmp=pattern;
	while((tmp=strchr(tmp,';'))){
		tmp++;
		N++;
	}
	Warning(debug,"Spec length is %d",N);
	pol->pattern=RTmalloc(N*sizeof(char*));
	pol->value=RTmalloc(N*sizeof(char*));
	for(int i=0;i<N-1;i++){
		pol->pattern[i]=pattern;
		tmp=strchr(pattern,';');
		tmp[0]=0;
		pattern=tmp+1;
		tmp=strrchr(pol->pattern[i],':');
		if (!tmp){
			Fatal(1,error,"bad map entry %s",pattern);
		}
		tmp[0]=0;
		pol->value[i]=tmp+1;
		Warning(debug,"map rule %d: %s -> %s",i,pol->pattern[i],pol->value[i]);
	}
	pol->pattern[N-1]=NULL;
	pol->value[N-1]=pattern;
	Warning(debug,"map default is %s",pol->value[N-1]);
	return pol;
}


char* SSMcall(string_map_t map,const char*input){
	if (map) {
		int i;
		for(i=0;map->pattern[i];i++){
			if (!fnmatch(map->pattern[i],input,0)) {
				break;
			}			
		}
		Warning(debug,"result for %s is %s",input,map->value[i]);
		return map->value[i];
	} else {
		Warning(debug,"empty map");
	}
	return NULL;
}


