/*
 * Copyright 2019 Alistair Leslie-Hughes
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define COBJMACROS

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "oledb.h"
#include "rpcproxy.h"
#include "wine/debug.h"

#include "initguid.h"
#include "msdasql.h"

#include "msdasql_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(msdasql);

DEFINE_GUID(DBPROPSET_DBINIT,    0xc8b522bc, 0x5cf3, 0x11ce, 0xad, 0xe5, 0x00, 0xaa, 0x00, 0x44, 0x77, 0x3d);

static HRESULT WINAPI ClassFactory_QueryInterface(IClassFactory *iface, REFIID riid, void **ppv)
{
    *ppv = NULL;

    if(IsEqualGUID(&IID_IUnknown, riid)) {
        TRACE("(%p)->(IID_IUnknown %p)\n", iface, ppv);
        *ppv = iface;
    }else if(IsEqualGUID(&IID_IClassFactory, riid)) {
        TRACE("(%p)->(IID_IClassFactory %p)\n", iface, ppv);
        *ppv = iface;
    }

    if(*ppv) {
        IUnknown_AddRef((IUnknown*)*ppv);
        return S_OK;
    }

    WARN("(%p)->(%s %p)\n", iface, debugstr_guid(riid), ppv);
    return E_NOINTERFACE;
}

static ULONG WINAPI ClassFactory_AddRef(IClassFactory *iface)
{
    TRACE("(%p)\n", iface);
    return 2;
}

static ULONG WINAPI ClassFactory_Release(IClassFactory *iface)
{
    TRACE("(%p)\n", iface);
    return 1;
}

static HRESULT create_msdasql_provider(REFIID riid, void **ppv);

HRESULT WINAPI msdasql_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **ppv)
{
    TRACE("(%p %s %p)\n", outer, debugstr_guid(riid), ppv);

    return create_msdasql_provider(riid, ppv);
}

static HRESULT WINAPI ClassFactory_LockServer(IClassFactory *iface, BOOL fLock)
{
    TRACE("(%p)->(%x)\n", iface, fLock);
    return S_OK;
}

static const IClassFactoryVtbl cfmsdasqlVtbl = {
    ClassFactory_QueryInterface,
    ClassFactory_AddRef,
    ClassFactory_Release,
    msdasql_CreateInstance,
    ClassFactory_LockServer
};

static IClassFactory cfmsdasql = { &cfmsdasqlVtbl };

HRESULT WINAPI DllGetClassObject( REFCLSID rclsid, REFIID riid, void **ppv )
{
    TRACE("%s %s %p\n", debugstr_guid(rclsid), debugstr_guid(riid), ppv);

    if (IsEqualGUID(&CLSID_MSDASQL, rclsid))
        return IClassFactory_QueryInterface(&cfmsdasql, riid, ppv);

    return CLASS_E_CLASSNOTAVAILABLE;
}

struct dbproperty
{
    const WCHAR *name;
    DBPROPID id;
    DBPROPOPTIONS options;
    VARTYPE type;
    HRESULT (*convert_dbproperty)(const WCHAR *src, VARIANT *dest);
};

struct mode_propval
{
    const WCHAR *name;
    DWORD value;
};

static int __cdecl dbmodeprop_compare(const void *a, const void *b)
{
    const WCHAR *src = a;
    const struct mode_propval *propval = b;
    return wcsicmp(src, propval->name);
}

static HRESULT convert_dbproperty_mode(const WCHAR *src, VARIANT *dest)
{
    struct mode_propval mode_propvals[] =
    {
        { L"Read", DB_MODE_READ },
        { L"ReadWrite", DB_MODE_READWRITE },
        { L"Share Deny None", DB_MODE_SHARE_DENY_NONE },
        { L"Share Deny Read", DB_MODE_SHARE_DENY_READ },
        { L"Share Deny Write", DB_MODE_SHARE_DENY_WRITE },
        { L"Share Exclusive", DB_MODE_SHARE_EXCLUSIVE },
        { L"Write", DB_MODE_WRITE },
    };
    struct mode_propval *prop;

    if ((prop = bsearch(src, mode_propvals, ARRAY_SIZE(mode_propvals),
                        sizeof(struct mode_propval), dbmodeprop_compare)))
    {
        V_VT(dest) = VT_I4;
        V_I4(dest) = prop->value;
        TRACE("%s = %#x\n", debugstr_w(src), prop->value);
        return S_OK;
    }

    return E_FAIL;
}

static const struct dbproperty dbproperties[] =
{
    { L"Password",                 DBPROP_AUTH_PASSWORD,                   DBPROPOPTIONS_OPTIONAL, VT_BSTR },
    { L"Persist Security Info",    DBPROP_AUTH_PERSIST_SENSITIVE_AUTHINFO, DBPROPOPTIONS_OPTIONAL, VT_BOOL },
    { L"User ID",                  DBPROP_AUTH_USERID,                     DBPROPOPTIONS_OPTIONAL, VT_BSTR },
    { L"Data Source",              DBPROP_INIT_DATASOURCE,                 DBPROPOPTIONS_REQUIRED, VT_BSTR },
    { L"Window Handle",            DBPROP_INIT_HWND,                       DBPROPOPTIONS_OPTIONAL, sizeof(void *) == 8 ? VT_I8 : VT_I4 },
    { L"Location",                 DBPROP_INIT_LOCATION,                   DBPROPOPTIONS_OPTIONAL, VT_BSTR },
    { L"Mode",                     DBPROP_INIT_MODE,                       DBPROPOPTIONS_OPTIONAL, VT_I4, convert_dbproperty_mode },
    { L"Prompt",                   DBPROP_INIT_PROMPT,                     DBPROPOPTIONS_OPTIONAL, VT_I2 },
    { L"Connect Timeout",          DBPROP_INIT_TIMEOUT,                    DBPROPOPTIONS_OPTIONAL, VT_I4 },
    { L"Extended Properties",      DBPROP_INIT_PROVIDERSTRING,             DBPROPOPTIONS_REQUIRED, VT_BSTR },
    { L"Locale Identifier",        DBPROP_INIT_LCID,                       DBPROPOPTIONS_OPTIONAL, VT_I4 },
    { L"Initial Catalog",          DBPROP_INIT_CATALOG,                    DBPROPOPTIONS_OPTIONAL, VT_BSTR },
    { L"OLE DB Services",          DBPROP_INIT_OLEDBSERVICES,              DBPROPOPTIONS_OPTIONAL, VT_I4 },
    { L"General Timeout",          DBPROP_INIT_GENERALTIMEOUT,             DBPROPOPTIONS_OPTIONAL, VT_I4 },
};

struct msdasql
{
    IUnknown         MSDASQL_iface;
    IDBProperties    IDBProperties_iface;
    IDBInitialize    IDBInitialize_iface;
    IDBCreateSession IDBCreateSession_iface;
    IPersist         IPersist_iface;

    LONG     ref;
};

static inline struct msdasql *impl_from_IUnknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct msdasql, MSDASQL_iface);
}

static inline struct msdasql *impl_from_IDBProperties(IDBProperties *iface)
{
    return CONTAINING_RECORD(iface, struct msdasql, IDBProperties_iface);
}

static inline struct msdasql *impl_from_IDBInitialize(IDBInitialize *iface)
{
    return CONTAINING_RECORD(iface, struct msdasql, IDBInitialize_iface);
}

static inline struct msdasql *impl_from_IDBCreateSession(IDBCreateSession *iface)
{
    return CONTAINING_RECORD(iface, struct msdasql, IDBCreateSession_iface);
}

static inline struct msdasql *impl_from_IPersist( IPersist *iface )
{
    return CONTAINING_RECORD( iface, struct msdasql, IPersist_iface );
}

static HRESULT WINAPI msdsql_QueryInterface(IUnknown *iface, REFIID riid, void **out)
{
    struct msdasql *provider = impl_from_IUnknown(iface);

    TRACE("(%p)->(%s %p)\n", provider, debugstr_guid(riid), out);

    if(IsEqualGUID(riid, &IID_IUnknown) ||
       IsEqualGUID(riid, &CLSID_MSDASQL))
    {
        *out = &provider->MSDASQL_iface;
    }
    else if(IsEqualGUID(riid, &IID_IDBProperties))
    {
        *out = &provider->IDBProperties_iface;
    }
    else if ( IsEqualGUID(riid, &IID_IDBInitialize))
    {
        *out = &provider->IDBInitialize_iface;
    }
    else if (IsEqualGUID(riid, &IID_IDBCreateSession))
    {
        *out = &provider->IDBCreateSession_iface;
    }
    else if(IsEqualGUID(&IID_IPersist, riid))
    {
        *out = &provider->IPersist_iface;
    }
    else
    {
        FIXME("(%s, %p)\n", debugstr_guid(riid), out);
        *out = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown*)*out);
    return S_OK;
}

static ULONG WINAPI msdsql_AddRef(IUnknown *iface)
{
    struct msdasql *provider = impl_from_IUnknown(iface);
    ULONG ref = InterlockedIncrement(&provider->ref);

    TRACE("(%p) ref=%u\n", provider, ref);

    return ref;
}

static ULONG  WINAPI msdsql_Release(IUnknown *iface)
{
    struct msdasql *provider = impl_from_IUnknown(iface);
    ULONG ref = InterlockedDecrement(&provider->ref);

    TRACE("(%p) ref=%u\n", provider, ref);

    if (!ref)
    {
        free(provider);
    }

    return ref;
}

static const IUnknownVtbl msdsql_vtbl =
{
    msdsql_QueryInterface,
    msdsql_AddRef,
    msdsql_Release
};

static HRESULT WINAPI dbprops_QueryInterface(IDBProperties *iface, REFIID riid, void **ppvObject)
{
    struct msdasql *provider = impl_from_IDBProperties(iface);

    return IUnknown_QueryInterface(&provider->MSDASQL_iface, riid, ppvObject);
}

static ULONG WINAPI dbprops_AddRef(IDBProperties *iface)
{
    struct msdasql *provider = impl_from_IDBProperties(iface);

    return IUnknown_AddRef(&provider->MSDASQL_iface);
}

static ULONG WINAPI dbprops_Release(IDBProperties *iface)
{
    struct msdasql *provider = impl_from_IDBProperties(iface);

    return IUnknown_Release(&provider->MSDASQL_iface);
}

static HRESULT WINAPI dbprops_GetProperties(IDBProperties *iface, ULONG cPropertyIDSets,
            const DBPROPIDSET rgPropertyIDSets[], ULONG *pcPropertySets, DBPROPSET **prgPropertySets)
{
    struct msdasql *provider = impl_from_IDBProperties(iface);

    FIXME("(%p)->(%d %p %p %p)\n", provider, cPropertyIDSets, rgPropertyIDSets, pcPropertySets, prgPropertySets);

    return E_NOTIMPL;
}

static HRESULT WINAPI dbprops_GetPropertyInfo(IDBProperties *iface, ULONG cPropertyIDSets,
            const DBPROPIDSET rgPropertyIDSets[], ULONG *pcPropertyInfoSets,
            DBPROPINFOSET **prgPropertyInfoSets, OLECHAR **ppDescBuffer)
{
    struct msdasql *provider = impl_from_IDBProperties(iface);
    int i;
    DBPROPINFOSET *infoset;
    int size = 1;
    OLECHAR *ptr;

    TRACE("(%p)->(%d %p %p %p %p)\n", provider, cPropertyIDSets, rgPropertyIDSets, pcPropertyInfoSets,
                prgPropertyInfoSets, ppDescBuffer);

    infoset = CoTaskMemAlloc(sizeof(DBPROPINFOSET));
    memcpy(&infoset->guidPropertySet, &DBPROPSET_DBINIT, sizeof(GUID));
    infoset->cPropertyInfos = ARRAY_SIZE(dbproperties);
    infoset->rgPropertyInfos = CoTaskMemAlloc(sizeof(DBPROPINFO) * ARRAY_SIZE(dbproperties));

    for(i=0; i < ARRAY_SIZE(dbproperties); i++)
    {
        size += lstrlenW(dbproperties[i].name) + 1;
    }

    ptr = *ppDescBuffer = CoTaskMemAlloc(size * sizeof(WCHAR));
    memset(*ppDescBuffer, 0, size * sizeof(WCHAR));

    for(i=0; i < ARRAY_SIZE(dbproperties); i++)
    {
        lstrcpyW(ptr, dbproperties[i].name);
        infoset->rgPropertyInfos[i].pwszDescription = ptr;
        infoset->rgPropertyInfos[i].dwPropertyID =  dbproperties[i].id;
        infoset->rgPropertyInfos[i].dwFlags = DBPROPFLAGS_DBINIT | DBPROPFLAGS_READ | DBPROPFLAGS_WRITE;
        infoset->rgPropertyInfos[i].vtType =  dbproperties[i].type;
        V_VT(&infoset->rgPropertyInfos[i].vValues) =  VT_EMPTY;

        ptr += lstrlenW(dbproperties[i].name) + 1;
    }

    *pcPropertyInfoSets = 1;
    *prgPropertyInfoSets = infoset;

    return S_OK;
}

static HRESULT WINAPI dbprops_SetProperties(IDBProperties *iface, ULONG cPropertySets,
            DBPROPSET rgPropertySets[])
{
    struct msdasql *provider = impl_from_IDBProperties(iface);

    FIXME("(%p)->(%d %p)\n", provider, cPropertySets, rgPropertySets);

    return E_NOTIMPL;
}

static const struct IDBPropertiesVtbl dbprops_vtbl =
{
    dbprops_QueryInterface,
    dbprops_AddRef,
    dbprops_Release,
    dbprops_GetProperties,
    dbprops_GetPropertyInfo,
    dbprops_SetProperties
};

static HRESULT WINAPI dbinit_QueryInterface(IDBInitialize *iface, REFIID riid, void **ppvObject)
{
    struct msdasql *provider = impl_from_IDBInitialize(iface);

    return IUnknown_QueryInterface(&provider->MSDASQL_iface, riid, ppvObject);
}

static ULONG WINAPI dbinit_AddRef(IDBInitialize *iface)
{
    struct msdasql *provider = impl_from_IDBInitialize(iface);

    return IUnknown_AddRef(&provider->MSDASQL_iface);
}

static ULONG WINAPI dbinit_Release(IDBInitialize *iface)
{
    struct msdasql *provider = impl_from_IDBInitialize(iface);

    return IUnknown_Release(&provider->MSDASQL_iface);
}

static HRESULT WINAPI dbinit_Initialize(IDBInitialize *iface)
{
    struct msdasql *provider = impl_from_IDBInitialize(iface);

    FIXME("%p stub\n", provider);

    return S_OK;
}

static HRESULT WINAPI dbinit_Uninitialize(IDBInitialize *iface)
{
    struct msdasql *provider = impl_from_IDBInitialize(iface);

    FIXME("%p stub\n", provider);

    return S_OK;
}


static const struct IDBInitializeVtbl dbinit_vtbl =
{
    dbinit_QueryInterface,
    dbinit_AddRef,
    dbinit_Release,
    dbinit_Initialize,
    dbinit_Uninitialize
};

static HRESULT WINAPI dbsess_QueryInterface(IDBCreateSession *iface, REFIID riid, void **ppvObject)
{
    struct msdasql *provider = impl_from_IDBCreateSession(iface);

    return IUnknown_QueryInterface(&provider->MSDASQL_iface, riid, ppvObject);
}

static ULONG WINAPI dbsess_AddRef(IDBCreateSession *iface)
{
    struct msdasql *provider = impl_from_IDBCreateSession(iface);

    return IUnknown_AddRef(&provider->MSDASQL_iface);
}

static ULONG WINAPI dbsess_Release(IDBCreateSession *iface)
{
    struct msdasql *provider = impl_from_IDBCreateSession(iface);

    return IUnknown_Release(&provider->MSDASQL_iface);
}

static HRESULT WINAPI dbsess_CreateSession(IDBCreateSession *iface, IUnknown *outer, REFIID riid,
        IUnknown **session)
{
    struct msdasql *provider = impl_from_IDBCreateSession(iface);
    HRESULT hr;

    TRACE("%p, outer %p, riid %s, session %p stub\n", provider, outer, debugstr_guid(riid), session);

    if (outer)
        FIXME("outer currently not supported.\n");

    hr = create_db_session(riid, (void**)session);

    return hr;
}

static const struct IDBCreateSessionVtbl dbsess_vtbl =
{
    dbsess_QueryInterface,
    dbsess_AddRef,
    dbsess_Release,
    dbsess_CreateSession
};

static HRESULT WINAPI persist_QueryInterface(IPersist *iface, REFIID riid, void **ppv)
{
    struct msdasql *provider = impl_from_IPersist( iface );
    return IUnknown_QueryInterface(&provider->MSDASQL_iface, riid, ppv);
}

static ULONG WINAPI persist_AddRef(IPersist *iface)
{
    struct msdasql *provider = impl_from_IPersist( iface );
    return IUnknown_AddRef(&provider->MSDASQL_iface);
}

static ULONG WINAPI persist_Release(IPersist *iface)
{
    struct msdasql *provider = impl_from_IPersist( iface );
    return IUnknown_Release(&provider->MSDASQL_iface);
}

static HRESULT WINAPI persist_GetClassID(IPersist *iface, CLSID *classid)
{
    struct msdasql *provider = impl_from_IPersist( iface );

    TRACE("(%p)->(%p)\n", provider, classid);

    if(!classid)
        return E_INVALIDARG;

    *classid = CLSID_MSDASQL;
    return S_OK;

}

static const IPersistVtbl persistVtbl = {
    persist_QueryInterface,
    persist_AddRef,
    persist_Release,
    persist_GetClassID
};

static HRESULT create_msdasql_provider(REFIID riid, void **ppv)
{
    struct msdasql *provider;
    HRESULT hr;

    provider = malloc(sizeof(struct msdasql));
    if (!provider)
        return E_OUTOFMEMORY;

    provider->MSDASQL_iface.lpVtbl = &msdsql_vtbl;
    provider->IDBProperties_iface.lpVtbl = &dbprops_vtbl;
    provider->IDBInitialize_iface.lpVtbl = &dbinit_vtbl;
    provider->IDBCreateSession_iface.lpVtbl = &dbsess_vtbl;
    provider->IPersist_iface.lpVtbl = &persistVtbl;
    provider->ref = 1;

    hr = IUnknown_QueryInterface(&provider->MSDASQL_iface, riid, ppv);
    IUnknown_Release(&provider->MSDASQL_iface);
    return hr;
}
