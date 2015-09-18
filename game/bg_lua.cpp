#if defined(PROJECT_GAME)
#include "g_local.h"
#elif defined(PROJECT_CGAME)
#include "cg_local.h"
#endif

#include "qcommon/q_shared.h"
#include "json/cJSON.h"

#include "bg_lua.h"
#include "bg_lualogger.h"
#include "bg_luaserialiser.h"

#define __STDC_FORMAT_MACROS // older compilers need this
#include <inttypes.h>
#ifdef JPLUA

#if defined(_MSC_VER) && !defined(SCONS_BUILD)
#pragma comment( lib, "lua" )
#endif

const uint32_t JPLUA_VERSION = 12;

static const char *baseDir = "lua/";
#if defined(PROJECT_GAME)
static const char *pluginDir = "lua/sv/";
#elif defined(PROJECT_CGAME)
static const char *pluginDir = "lua/cl/";
#endif
#define JPLUA_EXTENSION ".lua"

jplua_t JPLua;
extern std::unordered_map<int, lua_weapon> weapon_func_list;

#ifdef PROJECT_GAME
std::unordered_map<std::string, int> jplua_client_commands;
std::unordered_map<std::string, int> jplua_server_commands;
#elif defined PROJECT_CGAME
std::unordered_map<std::string, int> jplua_console_commands;
std::unordered_map<std::string, int> jplua_server_commands;
#endif

qboolean JPLua_IteratePlugins( jplua_plugin_t **plugin ) {
	if ( !*plugin )
		*plugin = JPLua.plugins;
	else
		*plugin = (*plugin)->next;

	JPLua.currentPlugin = *plugin;

	return (*plugin != NULL) ? qtrue : qfalse;
}

// stateless version of the above - does not set JPLua.currentPlugin
qboolean JPLua_IteratePluginsTemp( jplua_plugin_t **plugin ) {
	if ( !*plugin )
		*plugin = JPLua.plugins;
	else
		*plugin = (*plugin)->next;

	return (*plugin != NULL) ? qtrue : qfalse;
}

void JPLua_DPrintf( const char *msg, ... ) {
#ifdef JPLUA_DEBUG
	va_list argptr;
	char text[1024] = {0};

	va_start( argptr, msg );
	Q_vsnprintf( text, sizeof( text ), msg, argptr );
	va_end( argptr );

	trap->Print( "%s", text );
#endif
}

// Lua calls this if it panics, it'll then terminate the server with exit(EXIT_FAILURE)
// This error should never happen in a clean release version of JA++!
static int JPLuaI_Error( lua_State *L ) {
	trap->Print( S_COLOR_RED"*************** JA++ LUA ERROR ***************\n" );
	trap->Print( S_COLOR_RED"unprotected error in call to Lua API (%s)\n", lua_tostring( L, -1 ) );
	return 0;
}

qboolean JPLua_Call( lua_State *L, int argCount, int resCount ) {
	if ( lua_pcall( L, argCount, resCount, 0 ) ) {
		trap->Print( S_COLOR_GREEN "JPLua: " S_COLOR_RED "Error: %s\n", lua_tostring( L, -1 ) );
		lua_pop( L, 1 );
		return qfalse;
	}
	return qtrue;
}

#define JPLUA_LOAD_CHUNKSIZE (1024)
typedef struct gfd_s {// JPLua File Data
	fileHandle_t f;
	int dataRemaining;
	char buff[JPLUA_LOAD_CHUNKSIZE];
} gfd_t;

// Called by the loader, never access it directly
static const char *JPLua_LoadFile_Reader( lua_State *L, void *ud, size_t *sz ) {
	gfd_t *gfd = (gfd_t *)ud;

	if ( !gfd->dataRemaining )
		return NULL;

	if ( gfd->dataRemaining >= JPLUA_LOAD_CHUNKSIZE ) {
		trap->FS_Read( gfd->buff, JPLUA_LOAD_CHUNKSIZE, gfd->f );
		gfd->dataRemaining -= JPLUA_LOAD_CHUNKSIZE;
		*sz = JPLUA_LOAD_CHUNKSIZE;
		return gfd->buff;
	}
	else {
		trap->FS_Read( gfd->buff, gfd->dataRemaining, gfd->f );
		*sz = gfd->dataRemaining;
		gfd->dataRemaining = 0;
		return gfd->buff;
	}
}

// Loads a file using JA's FS functions, only use THIS to load files into lua!
int JPLua_LoadFile( lua_State *L, const char *file ) {
	fileHandle_t f = 0;
	int len = trap->FS_Open( file, &f, FS_READ );
	gfd_t gfd;
	int status;

	// file doesn't exist
	if ( !f || len <= 0 ) {
		trap->Print( "JPLua_LoadFile: Failed to load %s, file doesn't exist\n", file );
		return 1;
	}
	gfd.f = f;
	gfd.dataRemaining = len;

	status = (lua_load( L, JPLua_LoadFile_Reader, &gfd, va( "@%s", file ), NULL ) || lua_pcall( L, 0, 0, 0 ));
	if ( status ) {
		trap->Print( "JPLua_LoadFile: Failed to load %s: %s\n", file, lua_tostring( L, -1 ) );
		lua_pop( L, 1 );
	}

	trap->FS_Close( f );
	return status;
}

int JPLua_StackDump( lua_State *L ) {
	int i, top = lua_gettop( L );

	// repeat for each level
	for ( i = 1; i <= top; i++ ) {
		int t = lua_type( L, i );
		switch ( t ) {
		case LUA_TSTRING:
			trap->Print( "`%s'", lua_tostring( L, i ) );
			break;
		case LUA_TBOOLEAN:
			trap->Print( lua_toboolean( L, i ) ? "true" : "false" );
			break;
		case LUA_TNUMBER:
			trap->Print( LUA_NUMBER_FMT, lua_tonumber( L, i ) );
			break;
		default:
			trap->Print( "%s", lua_typename( L, t ) );
			break;
		}
		trap->Print( "  " );
	}
	trap->Print( "\n" );

	return 0;
}

// Framework functions constants
typedef enum jpLuaConsts_e {
	JPLUA_FRAMEWORK_TOSTRING,
	JPLUA_FRAMEWORK_PAIRS,
	JPLUA_FRAMEWORK_MAX,
} jpLuaConsts_t;

static int JPLua_Framework[JPLUA_FRAMEWORK_MAX]; // Contains lua references to framework functions, if any of these are 0 after init, we got a serious problem

int JPLua_Push_ToString( lua_State *L ) {
	lua_rawgeti( L, LUA_REGISTRYINDEX, JPLua_Framework[JPLUA_FRAMEWORK_TOSTRING] );
	return 1;
}

int JPLua_Push_Pairs( lua_State *L ) {
	lua_rawgeti( L, LUA_REGISTRYINDEX, JPLua_Framework[JPLUA_FRAMEWORK_PAIRS] );
	return 1;
}

void JPLua_Util_ArgAsString( lua_State *L, char *out, int bufsize ) {
	int args = lua_gettop( L );
	const char *res;
	int i;

	// Lets do this a lil different, concat all args and use that as the message ^^
	JPLua_Push_ToString( L ); // Ref to tostring (instead of a global lookup, in case someone changes it)

	for ( i = 1; i <= args; i++ ) {
		lua_pushvalue( L, -1 );
		lua_pushvalue( L, i );
		lua_call( L, 1, 1 ); // Assume this will never error out
		res = lua_tostring( L, -1 );
		if ( res )
			Q_strcat( out, bufsize, res );
		lua_pop( L, 1 );
	}
	lua_pop( L, 1 );

	return;
}

int propertycmp(const void *a, const void *b) {
	return strcmp((const char *)a, ((luaProperty_t *)b)->name);
}

extern int lastluaid;
static int JPLua_Export_Print( lua_State *L ) {
	char buf[16384] = { 0 };

	JPLua_Util_ArgAsString( L, buf, sizeof(buf) );
#ifdef PROJECT_GAME
	if (lastluaid == -1){
#endif
		trap->Print("%s\n", buf);
#ifdef PROJECT_GAME
	}
	else{
		trap->SendServerCommand(lastluaid,va("print \"%s\n\"", buf));
	}
#endif
	return 0;
}

static const char *colourComponents[] = { "r", "g", "b", "a" };
void JPLua_ReadColour( float *out, int numComponents, lua_State *L, int idx ) {
	int i = 0;

	for ( i = 0; i < numComponents; i++ ) {
		lua_getfield( L, idx, colourComponents[i] );
		out[i] = lua_tonumber( L, -1 );
	}
}

void JPLua_ReadFloats( float *out, int numComponents, lua_State *L, int idx ) {
	int i = 0;

	if ( lua_type( L, idx ) != LUA_TTABLE ) {
		trap->Print( "JPLua_ReadFloats failed, not a table\n" );
		JPLua_StackDump( L );
		return;
	}

	lua_pushnil( L );
	for ( i = 0; i < numComponents && lua_next( L, idx ); i++ ) {
		out[i] = lua_tonumber( L, -1 );
		lua_pop( L, 1 );
	}
}

void JPLua_PushInfostring( lua_State *L, const char *info ) {
	const char *s;
	char key[BIG_INFO_KEY], value[BIG_INFO_VALUE];
	int top = 0;

	lua_newtable( L );
	top = lua_gettop( L );

	//RAZTODO: cache userinfo somehow :/
	s = info;
	while ( s ) {
		Info_NextPair( &s, key, value );

		if ( !key[0] )
			break;

		lua_pushstring( L, key ); lua_pushstring( L, value ); lua_settable( L, top );
	}
}

void JPLua_PopInfostring( lua_State *L, char *info ) {
	lua_pushnil( L );
	while ( lua_next( L, -2 ) ) {
		Info_SetValueForKey( info, lua_tostring( L, -2 ), lua_tostring( L, -1 ) );
		lua_pop( L, 1 );
	}
	lua_pop( L, 1 );
}

static int JPLua_Export_Require( lua_State *L ) {
	const char *path = va( "%s%s", JPLua.initialised ? pluginDir : baseDir, lua_tostring( L, 1 ) );
	JPLua_LoadFile( L, path );
	return 0;
}

static int JPLua_RegisterPlugin( lua_State *L ) {
	int top;

	Q_strncpyz( JPLua.currentPlugin->longname, lua_tostring( L, 1 ), sizeof(JPLua.currentPlugin->longname) );
	Q_CleanString( JPLua.currentPlugin->longname, STRIP_COLOUR );
	Q_strncpyz( JPLua.currentPlugin->version, lua_tostring( L, 2 ), sizeof(JPLua.currentPlugin->version) );
	Q_CleanString( JPLua.currentPlugin->version, STRIP_COLOUR );
	JPLua.currentPlugin->requiredJPLuaVersion = lua_isnumber( L, 3 ) ? lua_tointeger( L, 3 ) : JPLUA_VERSION;
	JPLua.currentPlugin->UID = (intptr_t)JPLua.currentPlugin;

	lua_newtable( L );
	top = lua_gettop( L );

	lua_pushstring( L, "name" );
		lua_pushstring( L, JPLua.currentPlugin->longname );
		lua_settable( L, top );
	lua_pushstring( L, "version" );
		lua_pushstring( L, JPLua.currentPlugin->version );
		lua_settable( L, top );
	lua_pushstring( L, "UID" );
		lua_pushstring( L, va( "0x%" PRIxPTR, (void *)JPLua.currentPlugin->UID ) );
		lua_settable( L, top );
	lua_pushstring( L, "dirname" );
		lua_pushstring( L, JPLua.currentPlugin->name );
		lua_settable( L, top );

	//save in the registry, but push on stack again straight away
	JPLua.currentPlugin->handle = luaL_ref( L, LUA_REGISTRYINDEX );
	lua_rawgeti( L, LUA_REGISTRYINDEX, JPLua.currentPlugin->handle );

	return 1;
}

static void JPLua_LoadPlugin( const char *pluginName, const char *fileName ) {
	if ( !JPLua.plugins ) {
		// first plugin
		JPLua.plugins = (jplua_plugin_t *)malloc( sizeof(jplua_plugin_t) );
		JPLua.currentPlugin = JPLua.plugins;
	}
	else {
		JPLua.currentPlugin->next = (jplua_plugin_t *)malloc( sizeof(jplua_plugin_t) );
		JPLua.currentPlugin = JPLua.currentPlugin->next;
	}

	memset( JPLua.currentPlugin, 0, sizeof(jplua_plugin_t) );
	Q_strncpyz( JPLua.currentPlugin->longname, "<null>", sizeof(JPLua.currentPlugin->longname) );
	Q_strncpyz( JPLua.currentPlugin->name, pluginName, sizeof(JPLua.currentPlugin->name) );
	JPLua_LoadFile( JPLua.state, va( "%s%s/%s", pluginDir, pluginName, fileName ) );

	if ( JPLua.currentPlugin->requiredJPLuaVersion > JPLUA_VERSION ) {
		jplua_plugin_t *nextPlugin = JPLua.currentPlugin->next;
		trap->Print( "%s requires JPLua version %i\n", pluginName, JPLua.currentPlugin->requiredJPLuaVersion );
		luaL_unref( JPLua.state, LUA_REGISTRYINDEX, JPLua.currentPlugin->handle );
		free( JPLua.currentPlugin );
		if ( JPLua.plugins == JPLua.currentPlugin )
			JPLua.plugins = nextPlugin;
		JPLua.currentPlugin = nextPlugin;
	}
	else {
		trap->Print( "%-15s%-32s%-8s0x%" PRIxPTR "\n", "Loaded plugin:", JPLua.currentPlugin->longname,
			JPLua.currentPlugin->version, (void*)JPLua.currentPlugin->UID );
	}
}

static int JPLua_System_Index( lua_State *L ) {
	const char *key = lua_tostring( L, 2 );

	// see if this key is a function/constant in the metatable
	lua_getmetatable( L, 1 );
	lua_getfield( L, -1, key );
	if ( !lua_isnil( L, -1 ) )
		return 1;

	if ( !strcmp( key, "version" ) )
		lua_pushinteger( L, JPLUA_VERSION );
	else
		lua_pushnil( L );

	return 1;
}
static int JPLua_System_NewIndex( lua_State *L ) {
	return luaL_error( L, "Attempt to modify read-only data (JPLua)" );
}
static const struct luaL_Reg jplua_system_meta[] = {
	{ "__index", JPLua_System_Index },
	{ "__newindex", JPLua_System_NewIndex },
	{ NULL, NULL }
};
static void JPLua_Register_System( lua_State *L ) {
	const luaL_Reg *r;

	// register the metatable
	luaL_newmetatable( L, "JPLua.Meta" );
	for ( r = jplua_system_meta; r->name; r++ ) {
		lua_pushcfunction( L, r->func );
		lua_setfield( L, -2, r->name );
	}
	lua_pop( L, -1 );

	// create a global table named JPLua
	lua_newtable( L );
	luaL_getmetatable( L, "JPLua.Meta" );
	lua_setmetatable( L, -2 );
	lua_setglobal( L, "JPLua" );
}

static void JPLua_LoadPluginDir( qboolean inPK3 ) {
	static char folderList[16384];
	char *folderName = folderList;
	int i, numFolders;

	memset( folderList, 0, sizeof(folderList) );
	numFolders = trap->FS_GetFileList( pluginDir, inPK3 ? "" : "/", folderList, sizeof(folderList) );
	for ( i = 0; i < numFolders; i++ ) {
		size_t skipLenFolder = inPK3 ? 1 : 0, folderLen = 0;
		qboolean skip = qfalse;
		char *s;
		jplua_plugin_t *plugin = NULL;

		if ( folderName[0] == '.' )
			skip = qtrue;

		// check for loading the same plugin twice
		// this can happen when listing plugins outside of PK3s when plugins have written files using the Serialiser
		while ( JPLua_IteratePluginsTemp( &plugin ) ) {
			if ( !Q_stricmp( folderName, plugin->name ) )
				skip = qtrue;
		}

		if ( (s = (char *)Q_strchrs( folderName, "/\\" )) ) {
			if ( !s[1] )
				skip = qtrue;
			*s = '\0';
			skipLenFolder = strlen( ++s ) + 1;
		}
		folderLen = strlen( folderName ) + 1;
		if ( !skip ) {
			static char fileList[16384];
			char *fileName = fileList;
			int j, numFiles;

			memset( fileList, 0, sizeof(fileList) );
			numFiles = trap->FS_GetFileList( va( "%s%s", pluginDir, folderName ), JPLUA_EXTENSION, fileList, sizeof(fileList) );

			for ( j = 0; j < numFiles; j++ ) {
				size_t skipLenFile = inPK3 ? 1 : 0, fileLen = 0;
				if ( (s = (char *)Q_strchrs( fileName, "/\\" )) ) {
					*s = '\0';
					skipLenFile = strlen( ++s ) + 1;
				}
				fileLen = strlen( fileName ) + 1;
				if ( !Q_stricmp( fileName, "plugin" JPLUA_EXTENSION ) ) {
					JPLua_LoadPlugin( folderName, fileName );
					break;
				}
				fileName += fileLen + skipLenFile;
			}
		}
		folderName += folderLen + skipLenFolder;
	}
}

static void JPLua_PostInit( lua_State *L ) {
#if defined(PROJECT_GAME)
	trap->Print( S_COLOR_CYAN "**************** " S_COLOR_YELLOW "JA++ Lua (SV) is initialising " S_COLOR_CYAN "****************\n" );
#elif defined(PROJECT_CGAME)
	trap->Print( S_COLOR_CYAN "**************** " S_COLOR_YELLOW "JA++ Lua (CL) is initialising " S_COLOR_CYAN "****************\n" );
#endif

	JPLua_Register_System( L );
	JPLua_LoadFile( L, va( "%sinit" JPLUA_EXTENSION, baseDir ) );
	JPLua.initialised = qtrue;

	trap->Print( "%-15s%-32s%-8s%s\n", "               ", "Name", "Version", "Unique ID" );
	//TODO: handle duplicates, i.e. one in pk3, one out of pk3
	JPLua_LoadPluginDir( qtrue );
	JPLua_LoadPluginDir( qfalse );

#if defined(PROJECT_GAME)
	trap->Print( S_COLOR_CYAN "**************** " S_COLOR_GREEN "JA++ Lua (SV) is initialised " S_COLOR_GREEN "****************\n" );
#elif defined(PROJECT_CGAME)
	trap->Print( S_COLOR_CYAN "**************** " S_COLOR_GREEN "JA++ Lua (CL) is initialised " S_COLOR_GREEN "****************\n" );
#endif
}

#ifdef PROJECT_GAME
static int JPLua_Export_AddClientCommand( lua_State *L ) {
	const char *name = luaL_checkstring( L, 1 );
	int &handle = jplua_client_commands[name];

	if ( handle ) {
		// already exists
		trap->Print( "JPlua: AddClientCommand(%s) failed, command already exists. Remove command first\n", name );
		return 0;
	}

	const int top = lua_gettop( L );
	int typeArg2 = lua_type( L, 2 );

	if ( top == 2 ) {
		if ( typeArg2 == LUA_TFUNCTION ) {
			handle = luaL_ref( L, LUA_REGISTRYINDEX );
		}
		else {
			trap->Print( "JPLua: AddClientCommand(%s) failed, function signature invalid. Is it up to date?\n", name );
			return 0;
		}
	}
	else {
		trap->Print( "JPLua: AddClientCommand(%s) failed, too many arguments\n", name );
		return 0;
	}

	return 0;
}

#endif

#ifdef PROJECT_CGAME
static int JPLua_Export_AddConsoleCommand( lua_State *L ) {
	const char *name = luaL_checkstring( L, 1 );
	int &handle = jplua_console_commands[name];
	if ( handle ) {
		// already exists
		trap->Print( "JPlua: AddConsoleCommand(%s) failed, command already exists. Remove command first\n", name );
		return 0;
	}

	const int top = lua_gettop( L );
	int typeArg2 = lua_type( L, 2 );
	if ( top == 1 || (top == 2 && typeArg2 == LUA_TNIL) ) {
		// add to autocomplete list
		trap->AddCommand( name );
	}
	else if ( top == 2 ) {
		if ( typeArg2 == LUA_TFUNCTION ) {
			trap->AddCommand( name );
			handle = luaL_ref( L, LUA_REGISTRYINDEX );
		}
		else {
			trap->Print( "JPLua: AddConsoleCommand(%s) failed, function signature invalid. Is it up to date?\n", name );
			return 0;
		}
	}
	else {
		trap->Print( "JPLua: AddConsoleCommand(%s) failed, too many arguments\n", name );
		return 0;
	}

	return 0;
}
#endif

static int JPLua_Export_AddServerCommand( lua_State *L ) {
	const char *name = luaL_checkstring( L, 1 );
	int &handle = jplua_server_commands[name];
	if ( handle ) {
		// already exists
		trap->Print( "JPlua: AddServerCommand(%s) failed, command already exists. Remove command first\n", name );
	}

	if ( lua_type( L, 2 ) != LUA_TFUNCTION ) {
		trap->Print( "JPLua: AddServerCommand(%s) failed, function signature invalid. Is it up to date?\n", name );
		return 0;
	}

	handle = luaL_ref( L, LUA_REGISTRYINDEX );

	return 0;
}

#ifdef PROJECT_CGAME
static int JPLua_Export_DrawPic( lua_State *L ) {
	vector4 colour;
	int x, y, w, h, shader;

	x = lua_tointeger( L, 1 );
	y = lua_tointeger( L, 2 );
	w = lua_tointeger( L, 3 );
	h = lua_tointeger( L, 4 );
	JPLua_ReadFloats( colour.raw, 4, L, 5 );
	shader = lua_tointeger( L, 6 );

	trap->R_SetColor( &colour );
	CG_DrawPic( x, y, w, h, shader );
	trap->R_SetColor( NULL );

	return 0;
}
#endif

#ifdef PROJECT_CGAME
static int JPLua_Export_DrawRect( lua_State *L ) {
	vector4 colour = { 1.0f };

	JPLua_ReadFloats( colour.raw, 4, L, 5 );

	CG_FillRect( (float)lua_tonumber( L, 1 ), (float)lua_tonumber( L, 2 ), (float)lua_tonumber( L, 3 ),
		(float)lua_tonumber( L, 4 ), &colour );
	return 0;
}
#endif

#ifdef PROJECT_CGAME
static int JPLua_Export_DrawRotatedPic( lua_State *L ) {
	vector4 colour;
	int x, y, w, h, shader;
	float angle;

	x = lua_tointeger( L, 1 );
	y = lua_tointeger( L, 2 );
	w = lua_tointeger( L, 3 );
	h = lua_tointeger( L, 4 );
	angle = lua_tonumber( L, 5 );
	JPLua_ReadFloats( colour.raw, 4, L, 6 );
	shader = lua_tointeger( L, 7 );

	trap->R_SetColor( &colour );
	CG_DrawRotatePic2( x, y, w, h, angle, shader );
	trap->R_SetColor( NULL );

	return 0;
}
#endif

#ifdef PROJECT_CGAME
static int JPLua_Export_DrawText( lua_State *L ) {
	vector4 colour = { 1.0f };

	JPLua_ReadFloats( colour.raw, 4, L, 4 );

	CG_Text_Paint( (float)lua_tonumber( L, 1 ), (float)lua_tonumber( L, 2 ), (float)lua_tonumber( L, 5 ), &colour,
		lua_tostring( L, 3 ), 0.0f, 0, lua_tointeger( L, 6 ), lua_tointeger( L, 7 ), lua_toboolean(L, 8) );

	return 0;
}
#endif

#ifdef PROJECT_CGAME
static int JPLua_Export_Font_StringHeightPixels( lua_State *L ) {
	const char *text = lua_tostring(L, 1);
	float scale = lua_tonumber(L, 2);
	qhandle_t font = lua_tointeger(L, 3);
	qboolean customfont = lua_toboolean(L, 4);

	if (customfont)
		trap->R_Font_HeightPixels(font, scale);
	else
		lua_pushnumber( L, CG_Text_Height( text, scale, font ) );
	return 1;
}
#endif

#ifdef PROJECT_CGAME
static int JPLua_Export_Font_StringLengthPixels( lua_State *L ) {
	const char *text = lua_tostring(L, 1);
	float scale = lua_tonumber(L, 2);
	qhandle_t font = lua_tointeger(L, 3);
	qboolean customfont = lua_toboolean(L, 4);

	if (customfont)
		trap->R_Font_StrLenPixels(text, font, scale);
	else
		lua_pushnumber(L, CG_Text_Width(text, scale, font));
	return 1;
}
#endif

#ifdef PROJECT_CGAME
static int JPLua_Export_GetFPS( lua_State *L ) {
	lua_pushinteger( L, cg.japp.fps );
	return 1;
}
#endif

#ifdef PROJECT_CGAME
static int JPLua_Export_GetKeyCatcher( lua_State *L ) {
	lua_pushinteger( L, trap->Key_GetCatcher() );
	return 1;
}
#endif

static int JPLua_Export_GetGameType( lua_State *L ) {
#if defined(PROJECT_GAME)

	lua_pushinteger( L, level.gametype );

#elif defined(PROJECT_CGAME)

	lua_pushinteger( L, cgs.gametype );

#endif
	return 1;
}

static int JPLua_Export_GetMap( lua_State *L ) {
#if defined(PROJECT_GAME)

	char mapname[MAX_CVAR_VALUE_STRING];
	COM_StripExtension( level.rawmapname, mapname, sizeof(mapname) );
	lua_pushstring( L, mapname );

#elif defined(PROJECT_CGAME)

	lua_pushstring( L, cgs.mapnameClean );

#endif

	return 1;
}

static int JPLua_Export_GetMapTime( lua_State *L ) {
#if defined(PROJECT_GAME)

	int msec = level.time - level.startTime;
	int seconds = msec / 1000;
	int mins = seconds / 60;
	seconds %= 60;
	//	msec %= 1000;
	lua_pushstring( L, va( "%02i:%02i", mins, seconds ) );
	return 1;

#elif defined(PROJECT_CGAME)

	int msec = 0, secs = 0, mins = 0, limitSec = cgs.timelimit * 60;

	msec = cg.time - cgs.levelStartTime;
	secs = msec / 1000;
	mins = secs / 60;

	if ( cgs.timelimit && (cg_drawTimer.bits & DRAWTIMER_COUNTDOWN) ) {// count down
		msec = limitSec * 1000 - (msec);
		secs = msec / 1000;
		mins = secs / 60;
	}

	secs %= 60;
	msec %= 1000;

	lua_pushinteger( L, mins );
	lua_pushinteger( L, secs );
	lua_pushinteger( L, msec );
	return 3;

#endif
}

#ifdef PROJECT_CGAME
static int JPLua_Export_GetMousePos( lua_State *L ) {
	if ( trap->Key_GetCatcher() & KEYCATCH_CGAME ) {
		int top;
		lua_newtable( L );
		top = lua_gettop( L );
		lua_pushstring( L, "x" ); lua_pushinteger( L, cgs.cursorX ); lua_settop( L, top );
		lua_pushstring( L, "y" ); lua_pushinteger( L, cgs.cursorY ); lua_settop( L, top );
	}
	else
		lua_pushnil( L );

	return 1;
}
#endif

#ifdef PROJECT_GAME
//TODO: client-side version of this
static int JPLua_Export_GetPlayers( lua_State *L ) {
	int top, i = 1, clNum;
	gentity_t *ent;

	lua_newtable( L );
	top = lua_gettop( L );

	for ( ent = g_entities, clNum = 0; clNum < level.maxclients; ent++, clNum++ ) {
		if ( !ent->inuse || ent->client->pers.connected == CON_DISCONNECTED )
			continue;
		lua_pushnumber( L, i++ );
		JPLua_Player_CreateRef( L, clNum );
		lua_settable( L, top );
	}

	return 1;
}
#endif

static int JPLua_Export_GetRealTime( lua_State *L ) {
	lua_pushinteger( L, trap->Milliseconds() );
	return 1;
}

static int JPLua_Export_GetTime( lua_State *L ) {
#if defined(PROJECT_GAME)
	lua_pushinteger( L, level.time );
#elif defined(PROJECT_CGAME)
	lua_pushinteger( L, cg.time );
#endif
	return 1;
}

int JPLua_Export_Trace( lua_State *L ) {
	trace_t tr;
	vector3 *start, *end, mins, maxs;
	float size;
	int skipNumber, mask;
	int top, top2, top3;

	start = JPLua_CheckVector(L, 1);

	size = lua_tonumber( L, 2 ) / 2.0f;
	VectorSet( &mins, size, size, size );
	VectorScale( &mins, -1.0f, &maxs );

	end = JPLua_CheckVector(L, 3);

	skipNumber = lua_tointeger( L, 4 );
	mask = lua_tointeger( L, 5 );

#if defined(PROJECT_GAME)
	trap->Trace( &tr, start, &mins, &maxs, end, skipNumber, mask, qfalse, 0, 0 );
#elif defined(PROJECT_CGAME)
	CG_Trace( &tr, start, &mins, &maxs, end, skipNumber, mask );
#endif

	lua_newtable( L );
	top = lua_gettop( L );
	lua_pushstring( L, "allsolid" ); lua_pushboolean( L, !!tr.allsolid ); lua_settable( L, top );
	lua_pushstring( L, "startsolid" ); lua_pushboolean( L, !!tr.startsolid ); lua_settable( L, top );
	lua_pushstring( L, "entityNum" ); lua_pushinteger( L, tr.entityNum ); lua_settable( L, top );
	lua_pushstring( L, "fraction" ); lua_pushnumber( L, tr.fraction ); lua_settable( L, top );

	lua_pushstring( L, "endpos" );
	lua_newtable( L ); top2 = lua_gettop( L );
	lua_pushstring( L, "x" ); lua_pushnumber( L, tr.endpos.x ); lua_settable( L, top2 );
	lua_pushstring( L, "y" ); lua_pushnumber( L, tr.endpos.y ); lua_settable( L, top2 );
	lua_pushstring( L, "z" ); lua_pushnumber( L, tr.endpos.z ); lua_settable( L, top2 );
	lua_settable( L, top );

	lua_pushstring( L, "plane" );
	lua_newtable( L ); top2 = lua_gettop( L );
	lua_pushstring( L, "normal" );
	lua_newtable( L ); top3 = lua_gettop( L );
	lua_pushstring( L, "x" ); lua_pushnumber( L, tr.plane.normal.x ); lua_settable( L, top3 );
	lua_pushstring( L, "y" ); lua_pushnumber( L, tr.plane.normal.y ); lua_settable( L, top3 );
	lua_pushstring( L, "z" ); lua_pushnumber( L, tr.plane.normal.z ); lua_settable( L, top3 );
	lua_settable( L, top2 );
	lua_pushstring( L, "dist" ); lua_pushnumber( L, tr.plane.dist ); lua_settable( L, top2 );
	lua_pushstring( L, "type" ); lua_pushinteger( L, tr.plane.type ); lua_settable( L, top2 );
	lua_pushstring( L, "signbits" ); lua_pushinteger( L, tr.plane.signbits ); lua_settable( L, top2 );
	lua_settable( L, top );

	lua_pushstring( L, "surfaceFlags" ); lua_pushinteger( L, tr.surfaceFlags ); lua_settable( L, top );
	lua_pushstring( L, "contents" ); lua_pushinteger( L, tr.contents ); lua_settable( L, top );
	return 1;
}

#ifdef PROJECT_CGAME
static int JPLua_Export_RegisterShader( lua_State *L ) {
	lua_pushinteger( L, trap->R_RegisterShader( lua_tostring( L, 1 ) ) );
	return 1;
}
#endif

#ifdef PROJECT_CGAME
static int JPLua_Export_RegisterSound( lua_State *L ) {
	lua_pushinteger( L, trap->S_RegisterSound( lua_tostring( L, 1 ) ) );
	return 1;
}
#endif

#ifdef PROJECT_CGAME
static int JPLua_Export_RemapShader( lua_State *L ) {
	trap->R_RemapShader( lua_tostring( L, 1 ), lua_tostring( L, 2 ), lua_tostring( L, 3 ) );
	return 0;
}
#endif

#ifdef PROJECT_CGAME
void CG_ChatBox_AddString( char *chatStr ); //cg_draw.c
static int JPLua_Export_SendChatText( lua_State *L ) {
	char text[MAX_SAY_TEXT] = { 0 };

	if ( !cg_teamChatsOnly.integer ) {
		Q_strncpyz( text, lua_tostring( L, -1 ), MAX_SAY_TEXT );
		lua_pop( L, 1 );
		CG_LogPrintf( cg.log.console, va( "%s\n", text ) );
		if ( cg_newChatbox.integer )
			CG_ChatboxAddMessage( text, qfalse, "normal" );
		else
			CG_ChatBox_AddString( text );
	}

	return 0;
}
#endif

static int JPLua_Export_SendConsoleCommand( lua_State *L ) {
#if defined(PROJECT_GAME)
	trap->SendConsoleCommand( lua_tointeger( L, 1 ), va( "%s\n", lua_tostring( L, 2 ) ) );
#elif defined(PROJECT_CGAME)
	trap->SendConsoleCommand( lua_tostring( L, 1 ) );
#endif
	return 0;
}

#ifdef PROJECT_GAME
static int JPLua_Export_SendReliableCommand( lua_State *L ) {
	trap->SendServerCommand( lua_tointeger( L, 1 ), lua_tostring( L, 2 ) );
	return 0;
}
#endif

#ifdef PROJECT_CGAME
static int JPLua_Export_SendServerCommand( lua_State *L ) {
	trap->SendClientCommand( lua_tostring( L, 1 ) );
	return 0;
}
#endif

#ifdef PROJECT_CGAME
static int JPLua_Export_SetKeyCatcher( lua_State *L ) {
	uint32_t catcherMask = (uint32_t)lua_tointeger( L, 1 );
	trap->Key_SetCatcher( catcherMask );
	return 0;
}
#endif

#ifdef PROJECT_CGAME
static int JPLua_Export_SetMousePos( lua_State *L ) {
	cgs.cursorX = lua_tointeger( L, 1 );
	cgs.cursorY = lua_tointeger( L, 2 );
	return 0;
}
#endif

#ifdef PROJECT_CGAME
static int JPLua_Export_StartLocalSound( lua_State *L ) {
	trap->S_StartLocalSound( lua_tonumber( L, 1 ), lua_tonumber( L, 2 ) );
	return 0;
}
#endif

#ifdef PROJECT_CGAME
int JPLua_Export_TestLine( lua_State *L ) {
	vector3 start, end;
	float radius;
	int time;
	uint32_t color;

	lua_getfield( L, 1, "x" ); start.x = lua_tonumber( L, -1 );
	lua_getfield( L, 1, "y" ); start.y = lua_tonumber( L, -1 );
	lua_getfield( L, 1, "z" ); start.z = lua_tonumber( L, -1 );

	time = lua_tonumber( L, 3 );
	color = lua_tonumber( L, 4 );
	radius = lua_tonumber( L, 5 );
	lua_getfield( L, 2, "x" ); end.x = lua_tonumber( L, -1 );
	lua_getfield( L, 2, "y" ); end.y = lua_tonumber( L, -1 );
	lua_getfield( L, 2, "z" ); end.z = lua_tonumber( L, -1 );

	CG_TestLine( &start, &end, time, color, radius );

	return 0;
}
#endif

#ifdef PROJECT_CGAME
static int JPLua_Export_WorldCoordToScreenCoord( lua_State *L ) {
	vector3 *v = JPLua_CheckVector( L, 1 );
	float x, y;
	if ( CG_WorldCoordToScreenCoordFloat( v, &x, &y ) ) {
		lua_pushnumber( L, x );
		lua_pushnumber( L, y );
		return 2;
	}
	else {
		lua_pushnil( L );
		return 1;
	}
}
#endif

#ifdef PROJECT_GAME
static int JPLua_ConnectToDB( lua_State *L ) {
#if defined(NO_SQL)
	lua_pushnil( L );
	return 1;
#else
	int type = luaL_checkinteger( L, 1 );
	switch ( type ) {
	case 1: ///MySQL
		return JPLua_MySQL_Open( L );
	case 2: ///SQLite
		return JPLua_SQLite_Open( L );
	default:
		lua_pushnil(L);
		return 1;
	}
#endif
}
#endif
static int JPLua_GetConfigString(lua_State *L){
	int type = luaL_checkinteger(L, 1);
#ifdef PROJECT_CGAME
	const char *info = CG_ConfigString(type);
#elif defined (PROJECT_GAME)
	char info[MAX_INFO_STRING] = {};
	trap->GetConfigstring(type, info, sizeof(info));
#endif
	if (!info[0]){
		lua_pushnil(L);
		return 1;
	}
	JPLua_PushInfostring(L, info);
	return 1;
}

#ifdef PROJECT_GAME
static int JPLua_SetConfigString(lua_State *L){
	int type = luaL_checkinteger(L, 1);
	const char *value = luaL_checkstring(L, 2);
	trap->SetConfigstring(type, value);
	return 0;
}
#endif

#ifdef PROJECT_CGAME
static int JPLua_GetGLConfig(lua_State *L){
	glconfig_t config;
	int top;

	trap->GetGlconfig(&config);

	lua_newtable(L);
	top = lua_gettop(L);
	lua_pushstring(L, "renderer"); lua_pushstring(L, config.renderer_string); lua_settable(L, top);
	lua_pushstring(L, "vendor"); lua_pushstring(L, config.vendor_string); lua_settable(L, top);
	lua_pushstring(L, "version"); lua_pushstring(L, config.version_string); lua_settable(L, top);
	lua_pushstring(L, "extensions"); lua_pushstring(L, config.extensions_string); lua_settable(L, top);

	lua_pushstring(L, "colorbits"); lua_pushinteger(L, config.colorBits); lua_settable(L, top);
	lua_pushstring(L, "depthbits"); lua_pushinteger(L, config.depthBits); lua_settable(L, top);
	lua_pushstring(L, "stencilBits"); lua_pushinteger(L, config.stencilBits); lua_settable(L, top);

	lua_pushstring(L, "width"); lua_pushinteger(L, config.vidWidth); lua_settable(L, top);
	lua_pushstring(L, "height"); lua_pushinteger(L, config.vidHeight); lua_settable(L, top);
	lua_pushstring(L, "frequency"); lua_pushinteger(L, config.displayFrequency); lua_settable(L, top);

	lua_pushstring(L, "fullscreen"); lua_pushboolean(L, config.isFullscreen); lua_settable(L, top);

	return 1;
}

static int JPLua_Export_RegisterFont(lua_State *L){
	lua_pushinteger(L, trap->R_RegisterFont(luaL_checkstring(L, 1)));
	return 1;
}
#endif

#ifdef PROJECT_GAME
static int JPLua_GetModelBounds(lua_State *L){
	vector3 mins, maxs;
	const char *name = luaL_checkstring(L, 1);
	G_GetModelBounds(name, &mins, &maxs);
	JPLua_Vector_CreateRef(L, mins.x, mins.y, mins.z);
	JPLua_Vector_CreateRef(L, maxs.x, maxs.y, maxs.z);
	return 2;
}

static int JPLua_EntitiesInRadius(lua_State *L){
	vector3 *origin = JPLua_CheckVector(L, 1);
	float radius = luaL_checknumber(L, 2);
	gentity_t	*entity_list[MAX_GENTITIES];
	int count;

	count =	G_RadiusList(origin, radius, NULL, qtrue, entity_list);

	lua_newtable(L);
	int top = lua_gettop(L);

	for (int i = 0; i < count; i++) {
		lua_pushinteger(L, i + 1);
		JPLua_Entity_CreateRef(L, entity_list[i]);
		lua_settable(L, top);
	}
	return 1;
}

static int JPLua_EntitiesInBox(lua_State *L){
	vector3 *mins = JPLua_CheckVector(L, 1);
	vector3 *maxs = JPLua_CheckVector(L, 2);
	int entityList[MAX_GENTITIES], numListedEntities;
	gentity_t *ent;
	numListedEntities = trap->EntitiesInBox(mins, maxs, entityList, MAX_GENTITIES);

	lua_newtable(L);
	int top = lua_gettop(L);

	for (int i = 0; i < numListedEntities; i++) {
		ent = &g_entities[entityList[i]];
			lua_pushinteger(L, i + 1);
		JPLua_Entity_CreateRef(L, ent);
		lua_settable(L, top);
	}
	return 1;
}

static int JPLua_ScreenShake(lua_State *L){
	vector3 *origin = JPLua_CheckVector(L, 1);
	float intense = luaL_checknumber(L, 2);
	int duration = luaL_checkinteger(L, 3);
	qboolean global = lua_toboolean(L, 4);

	G_ScreenShake(origin, NULL, intense, duration, global);
	return 0;
}
#endif


static const jplua_cimport_table_t JPLua_CImports[] = {
#ifdef PROJECT_GAME
	{ "AddClientCommand", JPLua_Export_AddClientCommand }, // AddClientCommand( string cmd )
#endif
#ifdef PROJECT_CGAME
	{ "AddConsoleCommand", JPLua_Export_AddConsoleCommand }, // AddConsoleCommand( string cmd )
#endif
	{ "AddListener", JPLua_Event_AddListener }, // AddListener( string name, function listener )
	{ "AddServerCommand", JPLua_Export_AddServerCommand }, // AddServerCommand( string cmd )
	{ "CreateCvar", JPLua_CreateCvar }, // Cvar CreateCvar( string name [, string value [, integer flags] ] )
#ifdef PROJECT_CGAME
	{ "CreateMenu", JPLua_Interface_CreateMenu },
#endif
#ifdef PROJECT_GAME
	{ "CreateEntity", JPLua_Entity_Create },
#endif
#ifdef PROJECT_GAME
	{ "ConnectToDB", JPLua_ConnectToDB}, // ConnectToDB ( int type(1 - MySQL , 2 - SQLite), ...) // SQLite (type, string path) || MySQL ( type, string host, string user, string db, string password, int port )
#endif
#ifdef PROJECT_CGAME
	{ "DrawPic", JPLua_Export_DrawPic }, // DrawPic( float x, float y, float width, float height, table { float r, float g, float b, float a }, integer shaderHandle )
	{ "DrawRect", JPLua_Export_DrawRect }, // DrawRect( float x, float y, float width, float height, table { float r, float g, float b, float a } )
	{ "DrawRotatedPic", JPLua_Export_DrawRotatedPic }, // DrawPic( float x, float y, float width, float height, float angle, table { float r, float g, float b, float a }, integer shaderHandle )
	{ "DrawText", JPLua_Export_DrawText }, // DrawText( float x, float y, string text, table { float r, float g, float b, float a }, float scale, integer fontStyle, integer fontIndex )
	{ "Font_StringHeightPixels", JPLua_Export_Font_StringHeightPixels }, // integer Font_StringHeightPixels( integer fontHandle, float scale )
	{ "Font_StringLengthPixels", JPLua_Export_Font_StringLengthPixels }, // integer Font_StringLengthPixels( string str, integer fontHandle, float scale )
#endif
#ifdef PROJECT_GAME
	{ "EntitiesInBox", JPLua_EntitiesInBox },
	{ "EntitiesInRadius", JPLua_EntitiesInRadius },
	{ "FindEntityByClassname", JPLua_FindEntityByClassName },
#endif
	{ "GetConfigString", JPLua_GetConfigString }, // table GetConfigString()
	{ "GetCvar", JPLua_GetCvar }, // Cvar GetCvar( string name )
	{ "GetEntity", JPLua_Entity_Get }, // GetEntity(num) or GetEntity() for full list
	{ "GetFileList", JPLua_File_GetFileList}, // table GetFileList(string path, string extension)
#ifdef PROJECT_CGAME
	{ "GetFPS", JPLua_Export_GetFPS }, // integer GetFPS()
#endif
	{ "GetGameType", JPLua_Export_GetGameType }, // integer GetGameType()
#ifdef PROJECT_CGAME
	{ "GetGLConfig", JPLua_GetGLConfig }, // table GetGLConfig()
	{ "GetKeyCatcher", JPLua_Export_GetKeyCatcher }, // integer GetKeyCatcher()
#endif
	{ "GetLogger", JPLua_GetLogger }, // Logger GetLogger( string filename )
	{ "GetMap", JPLua_Export_GetMap }, // string GetMap()
	{ "GetMapTime", JPLua_Export_GetMapTime }, // string GetMapTime()
#ifdef PROJECT_CGAME
	{ "GetMenu", JPLua_Interface_GetMenu },
	{ "GetMousePos", JPLua_Export_GetMousePos }, // [{x,y}, nil] GetMousePos
#endif
	{ "GetPlayer", JPLua_GetPlayer }, // Player GetPlayer( integer clientNum )
#ifdef PROJECT_GAME
	{ "GetModelBounds", JPLua_GetModelBounds },
	{ "GetPlayers", JPLua_Export_GetPlayers }, // {Player,...} GetPlayers()
#endif
	{ "GetPlayerTable", JPLua_Player_GetMetaTable }, // Player.meta GetPlayerTable()
	{ "GetRealTime", JPLua_Export_GetRealTime }, // integer GetRealTime()
	{ "GetSerialiser", JPLua_GetSerialiser }, // Serialiser GetSerialiser( string fileName )
#ifdef PROJECT_CGAME
	{ "GetServer", JPLua_GetServer }, // Server GetServer()
#endif
	{ "GetTime", JPLua_Export_GetTime }, // integer GetTime()
	{ "OpenFile", JPLua_File_Open},
	{ "RayTrace", JPLua_Export_Trace }, // traceResult Trace( stuff )
	{ "RegisterPlugin", JPLua_RegisterPlugin }, // plugin RegisterPlugin( string name, string version )
#ifdef PROJECT_CGAME
	{ "RegisterFont", JPLua_Export_RegisterFont }, // RegisterFont( string name)
	{ "RegisterShader", JPLua_Export_RegisterShader }, // integer RegisterShader( string path )
	{ "RegisterSound", JPLua_Export_RegisterSound }, // integer RegisterSound( string path )
	{ "RemapShader", JPLua_Export_RemapShader }, // RemapShader( string oldshader, string newshader, string timeoffset )
#endif
	{ "RemoveListener", JPLua_Event_RemoveListener }, // RemoveListener( string name )
#ifdef PROJECT_CGAME
	{ "SendChatText", JPLua_Export_SendChatText }, // SendChatText( string text )
#endif
#ifdef PROJECT_GAME
	{ "ScreenShake", JPLua_ScreenShake },
#endif
	{ "SendConsoleCommand", JPLua_Export_SendConsoleCommand }, // SendConsoleCommand( string command )
#ifdef PROJECT_GAME
	{ "SendReliableCommand", JPLua_Export_SendReliableCommand }, // SendReliableCommand( integer clientNum, string cmd )
#endif
#ifdef PROJECT_GAME
	{ "SetConfigString", JPLua_SetConfigString}, // SetConfigString(integer type ( CS_SYSTEMINFO + i ) , value
	{ "SetWeaponFireFunc", JPLua_Weapon_SetFireFunction },
	{ "SetWeaponAltFireFunc", JPLua_Weapon_SetAltFireFunction },
#endif
#ifdef PROJECT_CGAME
	{ "SendServerCommand", JPLua_Export_SendServerCommand }, // SendServerCommand( string command )
	{ "SetKeyCatcher", JPLua_Export_SetKeyCatcher }, // SetKeyCatcher( integer catcherMask )
	{ "SetMousePos", JPLua_Export_SetMousePos }, // SetMousePos( integer x, integer y )
#endif
	{ "StackDump", JPLua_StackDump },
#ifdef PROJECT_CGAME
	{ "StartLocalSound", JPLua_Export_StartLocalSound }, // StartLocalSound( integer soundHandle, integer channelNum )
	{ "TestLine", JPLua_Export_TestLine }, // traceResult Trace( stuff )
#endif
	{ "Vector3", JPLua_GetVector3 }, // Vector Vector3( [float x, float y, float z] )
#ifdef PROJECT_CGAME
	{ "WorldCoordToScreenCoord", JPLua_Export_WorldCoordToScreenCoord }, // { float x, float y } WorldCoordToScreenCoord( Vector3 pos )
#endif
};
static const size_t cimportsSize = ARRAY_LEN( JPLua_CImports );

#endif // JPLUA

// initialise the JPLua system
void JPLua_Init( void ) {
#ifdef JPLUA
	size_t i = 0;

#if defined(PROJECT_GAME)
	if ( !g_jplua.integer ) {
#elif defined(PROJECT_CGAME)
	if ( !cg_jplua.integer ) {
#endif
		return;
	}
	//Initialise and load base libraries
	memset( JPLua_Framework, -1, sizeof(JPLua_Framework) );
	JPLua.state = luaL_newstate();
	if ( !JPLua.state ) {
		//TODO: Fail gracefully
		return;
	}

	lua_atpanic(JPLua.state, JPLuaI_Error); // Set the function called in a Lua error
	luaL_openlibs(JPLua.state);
	luaopen_string(JPLua.state);

	// Get rid of libraries we don't need
	lua_pushnil( JPLua.state );	lua_setglobal( JPLua.state, LUA_LOADLIBNAME ); // No need for the package library
	lua_pushnil( JPLua.state );	lua_setglobal( JPLua.state, LUA_IOLIBNAME ); // We use JKA engine facilities for file-handling

	// There are some stuff in the base library that we don't want
	lua_pushnil( JPLua.state );	lua_setglobal( JPLua.state, "dofile" );
	lua_pushnil( JPLua.state );	lua_setglobal( JPLua.state, "loadfile" );
	lua_pushnil( JPLua.state );	lua_setglobal( JPLua.state, "load" );
	lua_pushnil( JPLua.state );	lua_setglobal( JPLua.state, "loadstring" );

	// Some libraries we want, but not certain elements in them
	lua_getglobal( JPLua.state, LUA_OSLIBNAME ); // The OS library has dangerous access to the system, remove some parts of it
	lua_pushstring( JPLua.state, "execute" );		lua_pushnil( JPLua.state ); lua_settable( JPLua.state, -3 );
	lua_pushstring( JPLua.state, "exit" );			lua_pushnil( JPLua.state ); lua_settable( JPLua.state, -3 );
	lua_pushstring( JPLua.state, "remove" );		lua_pushnil( JPLua.state ); lua_settable( JPLua.state, -3 );
	lua_pushstring( JPLua.state, "rename" );		lua_pushnil( JPLua.state ); lua_settable( JPLua.state, -3 );
	lua_pushstring( JPLua.state, "setlocale" );		lua_pushnil( JPLua.state ); lua_settable( JPLua.state, -3 );
	lua_pushstring( JPLua.state, "tmpname" );		lua_pushnil( JPLua.state ); lua_settable( JPLua.state, -3 );
	lua_pop( JPLua.state, 1 );

	// Redefine global functions
	lua_pushcclosure( JPLua.state, JPLua_Export_Print, 0 );	lua_setglobal( JPLua.state, "print" );
	lua_pushcclosure( JPLua.state, JPLua_Export_Require, 0 );	lua_setglobal( JPLua.state, "require" );

	for ( i = 0; i < cimportsSize; i++ )
		lua_register( JPLua.state, JPLua_CImports[i].name, JPLua_CImports[i].function );

	// Register our classes
	JPLua_Register_Player( JPLua.state );
	JPLua_Register_Entity(JPLua.state);
#ifdef PROJECT_CGAME
	JPLua_Register_Server( JPLua.state );
	JPLua_Register_Menu(JPLua.state);
	JPLua_Register_Item(JPLua.state);
#endif
	JPLua_Register_Cvar( JPLua.state );
	JPLua_Register_Logger( JPLua.state );
	JPLua_Register_Serialiser( JPLua.state );
	JPLua_Register_Vector( JPLua.state );
	JPLua_Register_File( JPLua.state );
#ifdef PROJECT_GAME
	#if !defined(NO_SQL)
		JPLua_Register_MySQL(JPLua.state);
		JPLua_Register_SQLite(JPLua.state);
	#endif
#endif

	// -- FRAMEWORK INITIALISATION begin
	lua_getglobal( JPLua.state, "tostring" );	JPLua_Framework[JPLUA_FRAMEWORK_TOSTRING] = luaL_ref( JPLua.state, LUA_REGISTRYINDEX );
	lua_getglobal( JPLua.state, "pairs" );		JPLua_Framework[JPLUA_FRAMEWORK_PAIRS] = luaL_ref( JPLua.state, LUA_REGISTRYINDEX );

	for ( i = 0; i < JPLUA_FRAMEWORK_MAX; i++ ) {
		if ( JPLua_Framework[i] < 0 )
			Com_Error( ERR_FATAL, "FATAL ERROR: Could not properly initialize the JPLua framework!\n" );
	}
	// -- FRAMEWORK INITIALISATION end

	//Call our base scripts
	JPLua_PostInit( JPLua.state );
#endif
}
void JPLua_Shutdown( qboolean restart ) {
#ifdef JPLUA
	if ( JPLua.state ) {
		jplua_plugin_t *nextPlugin = JPLua.plugins;

		JPLua_Event_Shutdown( restart );

		JPLua.currentPlugin = JPLua.plugins;
		while ( nextPlugin ) {
			for ( int i = JPLUA_EVENT_UNLOAD; i < JPLUA_EVENT_MAX; i++ ) {
				if ( JPLua.currentPlugin->eventListeners[i] ) {
					luaL_unref( JPLua.state, LUA_REGISTRYINDEX, JPLua.currentPlugin->eventListeners[i] );
				}
			}
			luaL_unref( JPLua.state, LUA_REGISTRYINDEX, JPLua.currentPlugin->handle );
			nextPlugin = JPLua.currentPlugin->next;

			free( JPLua.currentPlugin );
			JPLua.currentPlugin = nextPlugin;
		}

#ifdef PROJECT_GAME
		for ( auto &row : jplua_client_commands ) {
			luaL_unref( JPLua.state, LUA_REGISTRYINDEX, row.second );
		}
		jplua_client_commands.clear();
		weapon_func_list.clear();
#elif defined PROJECT_CGAME
		for ( auto& row : jplua_console_commands ) {
			luaL_unref( JPLua.state, LUA_REGISTRYINDEX, row.second );
		}
		jplua_console_commands.clear();
#endif
		for ( auto &row : jplua_server_commands ) {
			luaL_unref( JPLua.state, LUA_REGISTRYINDEX, row.second );
		}
		jplua_server_commands.clear();

		JPLua.plugins = JPLua.currentPlugin = NULL;

		lua_close( JPLua.state );
		JPLua.state = NULL;
		JPLua.initialised = qfalse;
	}
#endif
}
