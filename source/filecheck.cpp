#include <filecheck.hpp>
#include <main.hpp>
#include <GarrysMod/Lua/Interface.h>
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <symbolfinder.hpp>
#include <detours.h>
#include <networkstringtabledefs.h>
#include <strtools.h>

namespace filecheck
{

enum ValidationMode
{
	ValidationModeNone,
	ValidationModeFixed,
	ValidationModeLua
};

#if defined _WIN32

static const char IsValidFileForTransfer_sig[] = "\x55\x8B\xEC\x53\x8B\x5D\x08\x85\xDB\x0F\x84\x2A\x2A\x2A\x2A\x80";
static const size_t IsValidFileForTransfer_siglen = sizeof( IsValidFileForTransfer_sig ) - 1;

#elif defined __linux

static const char IsValidFileForTransfer_sig[] = "@_ZN8CNetChan22IsValidFileForTransferEPKc";
static const size_t IsValidFileForTransfer_siglen = 0;

#elif defined __APPLE__

static const char IsValidFileForTransfer_sig[] = "@_ZN8CNetChan22IsValidFileForTransferEPKc";
static const size_t IsValidFileForTransfer_siglen = 0;

#endif

typedef bool( *IsValidFileForTransfer_t )( const char *file );

static IsValidFileForTransfer_t IsValidFileForTransfer = nullptr;
static MologieDetours::Detour<IsValidFileForTransfer_t> *IsValidFileForTransfer_detour = nullptr;

static INetworkStringTable *downloads = nullptr;
static const char *downloads_dir = "downloads" CORRECT_PATH_SEPARATOR_S;

static GarrysMod::Lua::ILuaInterface *lua_interface = nullptr;
static ValidationMode validation_mode = ValidationModeNone;
static const char *hook_name = "IsValidFileForTransfer";

static bool PushDebugTraceback( GarrysMod::Lua::ILuaInterface *lua )
{
	lua->GetField( GarrysMod::Lua::INDEX_GLOBAL, "debug" );
	if( !lua->IsType( -1, GarrysMod::Lua::Type::TABLE ) )
	{
		lua->ErrorNoHalt( "[ServerSecure] Global debug is not a table!\n" );
		lua->Pop( 1 );
		return false;
	}

	lua->GetField( -1, "traceback" );
	lua->Remove( -2 );
	if( !lua->IsType( -1, GarrysMod::Lua::Type::FUNCTION ) )
	{
		lua->ErrorNoHalt( "[ServerSecure] Global debug.traceback is not a function!\n" );
		lua->Pop( 1 );
		return false;
	}

	return true;
}

inline bool PushHookRun( GarrysMod::Lua::ILuaInterface *lua )
{
	if( !PushDebugTraceback( lua ) )
		return false;

	lua->GetField( GarrysMod::Lua::INDEX_GLOBAL, "hook" );
	if( !lua->IsType( -1, GarrysMod::Lua::Type::TABLE ) )
	{
		lua->ErrorNoHalt( "[ServerSecure] Global hook is not a table!\n" );
		lua->Pop( 2 );
		return false;
	}

	lua->GetField( -1, "Run" );
	lua->Remove( -2 );
	if( !lua->IsType( -1, GarrysMod::Lua::Type::FUNCTION ) )
	{
		lua->ErrorNoHalt( "[ServerSecure] Global hook.Run is not a function!\n" );
		lua->Pop( 2 );
		return false;
	}

	return true;
}

inline bool BlockDownload( const char *filepath )
{
	DebugWarning( "[ServerSecure] Blocking download of \"%s\"\n", filepath );
	return false;
}

static bool IsValidFileForTransfer_d( const char *filepath )
{
	if( filepath == nullptr )
	{
		DebugWarning( "[ServerSecure] Invalid file to download (string pointer was NULL)\n" );
		return false;
	}

	size_t len = std::strlen( filepath );
	if( len == 0 )
	{
		DebugWarning( "[ServerSecure] Invalid file to download (path length was 0)\n" );
		return false;
	}

	if( validation_mode == ValidationModeLua )
	{
		if( !PushHookRun( lua_interface ) )
			return false;

		lua_interface->PushString( hook_name );
		lua_interface->PushString( filepath );

		bool valid = true;
		if( lua_interface->PCall( 2, 1, -4 ) != 0 )
			lua_interface->Msg( "\n[ERROR] %s\n\n", lua_interface->GetString( -1 ) );
		else if( lua_interface->IsType( -1, GarrysMod::Lua::Type::BOOL ) )
			valid = lua_interface->GetBool( -1 );

		lua_interface->Pop( 2 );

		return valid;
	}

	std::string nicefile( filepath, len );
	if( !V_RemoveDotSlashes( &nicefile[0] ) )
		return BlockDownload( filepath );

	len = std::strlen( nicefile.c_str( ) );
	nicefile.resize( len );
	filepath = nicefile.c_str( );

	DebugWarning( "[ServerSecure] Checking file \"%s\"\n", filepath );

	if( !IsValidFileForTransfer( filepath ) )
		return BlockDownload( filepath );

	int32_t index = downloads->FindStringIndex( filepath );
	if( index != INVALID_STRING_INDEX )
		return true;

	if( len == 22 && std::strncmp( filepath, downloads_dir, 10 ) == 0 && std::strncmp( filepath + len - 4, ".dat", 4 ) == 0 )
		return true;

	return BlockDownload( filepath );
}

inline bool SetDetourStatus( ValidationMode mode )
{
	if( mode != ValidationModeNone && IsValidFileForTransfer_detour == nullptr )
	{
		IsValidFileForTransfer_detour = new( std::nothrow ) MologieDetours::Detour<IsValidFileForTransfer_t>(
			IsValidFileForTransfer,
			IsValidFileForTransfer_d
		);
		if( IsValidFileForTransfer_detour != nullptr )
		{
			validation_mode = mode;
			return true;
		}
	}

	if( mode == ValidationModeNone && IsValidFileForTransfer_detour != nullptr )
	{
		delete IsValidFileForTransfer_detour;
		IsValidFileForTransfer_detour = nullptr;
		validation_mode = mode;
		return true;
	}

	return false;
}

LUA_FUNCTION_STATIC( EnableFileValidation )
{
	if( LUA->Top( ) < 1 )
		LUA->ArgError( 1, "boolean or number expected, got nil" );

	ValidationMode mode = ValidationModeFixed;
	if( LUA->IsType( 1, GarrysMod::Lua::Type::BOOL ) )
	{
		mode = LUA->GetBool( 1 ) ? ValidationModeFixed : ValidationModeNone;
	}
	else if( LUA->IsType( 1, GarrysMod::Lua::Type::NUMBER ) )
	{
		int32_t num = static_cast<int32_t>( LUA->GetNumber( 1 ) );
		if( num < 0 || num > 2 )
			LUA->ArgError( 1, "invalid mode value, must be 0, 1 or 2" );

		mode = static_cast<ValidationMode>( num );
	}
	else
	{
		LUA->ArgError( 1, "boolean or number expected" );
	}

	LUA->PushBool( SetDetourStatus( mode ) );
	return 1;
}

void Initialize( lua_State *state )
{
	lua_interface = static_cast<GarrysMod::Lua::ILuaInterface *>( LUA );

	INetworkStringTableContainer *networkstringtable = global::engine_loader.GetInterface<INetworkStringTableContainer>(
		INTERFACENAME_NETWORKSTRINGTABLESERVER
	);
	if( networkstringtable == nullptr )
		LUA->ThrowError( "unable to get INetworkStringTableContainer" );

	downloads = networkstringtable->FindTable( "downloadables" );
	if( downloads == nullptr )
		LUA->ThrowError( "missing \"downloadables\" string table" );

	SymbolFinder symfinder;
	IsValidFileForTransfer = reinterpret_cast<IsValidFileForTransfer_t>( symfinder.ResolveOnBinary(
		global::engine_lib.c_str( ),
		IsValidFileForTransfer_sig,
		IsValidFileForTransfer_siglen
	) );
	if( IsValidFileForTransfer == nullptr )
		LUA->ThrowError( "unable to sigscan for CNetChan::IsValidFileForTransfer" );
}

int32_t PostInitialize( lua_State *state )
{
	LUA->PushCFunction( EnableFileValidation );
	LUA->SetField( -2, "EnableFileValidation" );

	return 0;
}

void Deinitialize( lua_State * )
{
	if( IsValidFileForTransfer != nullptr )
	{
		delete IsValidFileForTransfer_detour;
		IsValidFileForTransfer_detour = nullptr;
	}
}

}
