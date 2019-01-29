/*
 * Process Hacker -
 *   LSA support functions
 *
 * Copyright (C) 2010-2011 wj32
 * Copyright (C) 2019 dmex
 *
 * This file is part of Process Hacker.
 *
 * Process Hacker is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Process Hacker is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Process Hacker.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * These are functions which communicate with LSA or are support functions. They replace certain
 * Win32 security-related functions such as LookupAccountName, LookupAccountSid and
 * LookupPrivilege*, which are badly designed. (LSA already allocates the return values for the
 * caller, yet the Win32 functions insist on their callers providing their own buffers.)
 */

#include <ph.h>
#include <lsasup.h>

NTSTATUS PhOpenLsaPolicy(
    _Out_ PLSA_HANDLE PolicyHandle,
    _In_ ACCESS_MASK DesiredAccess,
    _In_opt_ PUNICODE_STRING SystemName
    )
{
    OBJECT_ATTRIBUTES objectAttributes;

    InitializeObjectAttributes(&objectAttributes, NULL, 0, NULL, NULL);

    return LsaOpenPolicy(
        SystemName,
        &objectAttributes,
        DesiredAccess,
        PolicyHandle
        );
}

/**
 * Retrieves a handle to the local LSA policy with POLICY_LOOKUP_NAMES access.
 *
 * \remarks Do not close the handle; it is cached.
 */
LSA_HANDLE PhGetLookupPolicyHandle(
    VOID
    )
{
    static LSA_HANDLE cachedLookupPolicyHandle = NULL;
    LSA_HANDLE lookupPolicyHandle;
    LSA_HANDLE newLookupPolicyHandle;

    // Use the cached value if possible.

    lookupPolicyHandle = cachedLookupPolicyHandle;

    // If there is no cached handle, open one.

    if (!lookupPolicyHandle)
    {
        if (NT_SUCCESS(PhOpenLsaPolicy(
            &newLookupPolicyHandle,
            POLICY_LOOKUP_NAMES,
            NULL
            )))
        {
            // We succeeded in opening a policy handle, and since we did not have a cached handle
            // before, we will now store it.

            lookupPolicyHandle = _InterlockedCompareExchangePointer(
                &cachedLookupPolicyHandle,
                newLookupPolicyHandle,
                NULL
                );

            if (!lookupPolicyHandle)
            {
                // Success. Use our handle.
                lookupPolicyHandle = newLookupPolicyHandle;
            }
            else
            {
                // Someone already placed a handle in the cache. Close our handle and use their
                // handle.
                LsaClose(newLookupPolicyHandle);
            }
        }
    }

    return lookupPolicyHandle;
}

/**
 * Gets the name of a privilege from its LUID.
 *
 * \param PrivilegeValue The LUID of a privilege.
 * \param PrivilegeName A variable which receives a pointer to a string containing the privilege
 * name. You must free the string using PhDereferenceObject() when you no longer need it.
 */
BOOLEAN PhLookupPrivilegeName(
    _In_ PLUID PrivilegeValue,
    _Out_ PPH_STRING *PrivilegeName
    )
{
    NTSTATUS status;
    PUNICODE_STRING name;

    status = LsaLookupPrivilegeName(
        PhGetLookupPolicyHandle(),
        PrivilegeValue,
        &name
        );

    if (!NT_SUCCESS(status))
        return FALSE;

    *PrivilegeName = PhCreateStringFromUnicodeString(name);
    LsaFreeMemory(name);

    return TRUE;
}

/**
 * Gets the display name of a privilege from its name.
 *
 * \param PrivilegeName The name of a privilege.
 * \param PrivilegeDisplayName A variable which receives a pointer to a string containing the
 * privilege's display name. You must free the string using PhDereferenceObject() when you no longer
 * need it.
 */
BOOLEAN PhLookupPrivilegeDisplayName(
    _In_ PPH_STRINGREF PrivilegeName,
    _Out_ PPH_STRING *PrivilegeDisplayName
    )
{
    NTSTATUS status;
    UNICODE_STRING privilegeName;
    PUNICODE_STRING displayName;
    SHORT language;

    if (!PhStringRefToUnicodeString(PrivilegeName, &privilegeName))
        return FALSE;

    status = LsaLookupPrivilegeDisplayName(
        PhGetLookupPolicyHandle(),
        &privilegeName,
        &displayName,
        &language
        );

    if (!NT_SUCCESS(status))
        return FALSE;

    *PrivilegeDisplayName = PhCreateStringFromUnicodeString(displayName);
    LsaFreeMemory(displayName);

    return TRUE;
}

/**
 * Gets the LUID of a privilege from its name.
 *
 * \param PrivilegeName The name of a privilege.
 * \param PrivilegeValue A variable which receives the LUID of the privilege.
 */
BOOLEAN PhLookupPrivilegeValue(
    _In_ PPH_STRINGREF PrivilegeName,
    _Out_ PLUID PrivilegeValue
    )
{
    UNICODE_STRING privilegeName;

    if (!PhStringRefToUnicodeString(PrivilegeName, &privilegeName))
        return FALSE;

    return NT_SUCCESS(LsaLookupPrivilegeValue(
        PhGetLookupPolicyHandle(),
        &privilegeName,
        PrivilegeValue
        ));
}

/**
 * Gets information about a SID.
 *
 * \param Sid A SID to query.
 * \param Name A variable which receives a pointer to a string containing the SID's name. You must
 * free the string using PhDereferenceObject() when you no longer need it.
 * \param DomainName A variable which receives a pointer to a string containing the SID's domain
 * name. You must free the string using PhDereferenceObject() when you no longer need it.
 * \param NameUse A variable which receives the SID's usage.
 */
NTSTATUS PhLookupSid(
    _In_ PSID Sid,
    _Out_opt_ PPH_STRING *Name,
    _Out_opt_ PPH_STRING *DomainName,
    _Out_opt_ PSID_NAME_USE NameUse
    )
{
    NTSTATUS status;
    LSA_HANDLE policyHandle;
    PLSA_REFERENCED_DOMAIN_LIST referencedDomains;
    PLSA_TRANSLATED_NAME names;

    policyHandle = PhGetLookupPolicyHandle();

    referencedDomains = NULL;
    names = NULL;

    if (NT_SUCCESS(status = LsaLookupSids(
        policyHandle,
        1,
        &Sid,
        &referencedDomains,
        &names
        )))
    {
        if (names[0].Use != SidTypeInvalid && names[0].Use != SidTypeUnknown)
        {
            if (Name)
            {
                *Name = PhCreateStringFromUnicodeString(&names[0].Name);
            }

            if (DomainName)
            {
                if (names[0].DomainIndex >= 0)
                {
                    PLSA_TRUST_INFORMATION trustInfo;

                    trustInfo = &referencedDomains->Domains[names[0].DomainIndex];
                    *DomainName = PhCreateStringFromUnicodeString(&trustInfo->Name);
                }
                else
                {
                    *DomainName = PhReferenceEmptyString();
                }
            }

            if (NameUse)
            {
                *NameUse = names[0].Use;
            }
        }
        else
        {
            status = STATUS_NONE_MAPPED;
        }
    }

    // LsaLookupSids allocates memory even if it returns STATUS_NONE_MAPPED.
    if (referencedDomains)
        LsaFreeMemory(referencedDomains);
    if (names)
        LsaFreeMemory(names);

    return status;
}

/**
 * Gets information about a name.
 *
 * \param Name A name to query.
 * \param Sid A variable which receives a pointer to a SID. You must free the SID using PhFree()
 * when you no longer need it.
 * \param DomainName A variable which receives a pointer to a string containing the SID's domain
 * name. You must free the string using PhDereferenceObject() when you no longer need it.
 * \param NameUse A variable which receives the SID's usage.
 */
NTSTATUS PhLookupName(
    _In_ PPH_STRINGREF Name,
    _Out_opt_ PSID *Sid,
    _Out_opt_ PPH_STRING *DomainName,
    _Out_opt_ PSID_NAME_USE NameUse
    )
{
    NTSTATUS status;
    LSA_HANDLE policyHandle;
    UNICODE_STRING name;
    PLSA_REFERENCED_DOMAIN_LIST referencedDomains;
    PLSA_TRANSLATED_SID2 sids;

    if (!PhStringRefToUnicodeString(Name, &name))
        return STATUS_NAME_TOO_LONG;

    policyHandle = PhGetLookupPolicyHandle();
    referencedDomains = NULL;
    sids = NULL;

    if (NT_SUCCESS(status = LsaLookupNames2(
        policyHandle,
        0,
        1,
        &name,
        &referencedDomains,
        &sids
        )))
    {
        if (sids[0].Use != SidTypeInvalid && sids[0].Use != SidTypeUnknown)
        {
            if (Sid)
            {
                *Sid = PhAllocateCopy(sids[0].Sid, RtlLengthSid(sids[0].Sid));
            }

            if (DomainName)
            {
                if (sids[0].DomainIndex >= 0)
                {
                    PLSA_TRUST_INFORMATION trustInfo;

                    trustInfo = &referencedDomains->Domains[sids[0].DomainIndex];
                    *DomainName = PhCreateStringFromUnicodeString(&trustInfo->Name);
                }
                else
                {
                    *DomainName = PhReferenceEmptyString();
                }
            }

            if (NameUse)
            {
                *NameUse = sids[0].Use;
            }
        }
        else
        {
            status = STATUS_NONE_MAPPED;
        }
    }

    // LsaLookupNames2 allocates memory even if it returns STATUS_NONE_MAPPED.
    if (referencedDomains)
        LsaFreeMemory(referencedDomains);
    if (sids)
        LsaFreeMemory(sids);

    return status;
}

/**
 * Gets the name of a SID.
 *
 * \param Sid A SID to query.
 * \param IncludeDomain TRUE to include the domain name, otherwise FALSE.
 * \param NameUse A variable which receives the SID's usage.
 *
 * \return A pointer to a string containing the name of the SID in the following format:
 * domain\\name. You must free the string using PhDereferenceObject() when you no longer need it. If
 * an error occurs, the function returns NULL.
 */
PPH_STRING PhGetSidFullName(
    _In_ PSID Sid,
    _In_ BOOLEAN IncludeDomain,
    _Out_opt_ PSID_NAME_USE NameUse
    )
{
    NTSTATUS status;
    PPH_STRING fullName;
    LSA_HANDLE policyHandle;
    PLSA_REFERENCED_DOMAIN_LIST referencedDomains;
    PLSA_TRANSLATED_NAME names;

    policyHandle = PhGetLookupPolicyHandle();

    referencedDomains = NULL;
    names = NULL;

    if (NT_SUCCESS(status = LsaLookupSids(
        policyHandle,
        1,
        &Sid,
        &referencedDomains,
        &names
        )))
    {
        if (names[0].Use != SidTypeInvalid && names[0].Use != SidTypeUnknown)
        {
            PWSTR domainNameBuffer;
            ULONG domainNameLength;

            if (IncludeDomain && names[0].DomainIndex >= 0)
            {
                PLSA_TRUST_INFORMATION trustInfo;

                trustInfo = &referencedDomains->Domains[names[0].DomainIndex];
                domainNameBuffer = trustInfo->Name.Buffer;
                domainNameLength = trustInfo->Name.Length;
            }
            else
            {
                domainNameBuffer = NULL;
                domainNameLength = 0;
            }

            if (domainNameBuffer && domainNameLength != 0)
            {
                fullName = PhCreateStringEx(NULL, domainNameLength + sizeof(WCHAR) + names[0].Name.Length);
                memcpy(&fullName->Buffer[0], domainNameBuffer, domainNameLength);
                fullName->Buffer[domainNameLength / sizeof(WCHAR)] = OBJ_NAME_PATH_SEPARATOR;
                memcpy(&fullName->Buffer[domainNameLength / sizeof(WCHAR) + 1], names[0].Name.Buffer, names[0].Name.Length);
            }
            else
            {
                fullName = PhCreateStringFromUnicodeString(&names[0].Name);
            }

            if (NameUse)
            {
                *NameUse = names[0].Use;
            }
        }
        else
        {
            fullName = NULL;
        }
    }
    else
    {
        fullName = NULL;
    }

    if (referencedDomains)
        LsaFreeMemory(referencedDomains);
    if (names)
        LsaFreeMemory(names);

    return fullName;
}

/**
 * Gets a SDDL string representation of a SID.
 *
 * \param Sid A SID to query.
 *
 * \return A pointer to a string containing the SDDL representation of the SID. You must free the
 * string using PhDereferenceObject() when you no longer need it. If an error occurs, the function
 * returns NULL.
 */
PPH_STRING PhSidToStringSid(
    _In_ PSID Sid
    )
{
    PPH_STRING string;
    UNICODE_STRING us;

    string = PhCreateStringEx(NULL, SECURITY_MAX_SID_STRING_CHARACTERS * sizeof(WCHAR));
    PhStringRefToUnicodeString(&string->sr, &us);

    if (NT_SUCCESS(RtlConvertSidToUnicodeString(
        &us,
        Sid,
        FALSE
        )))
    {
        string->Length = us.Length;
        string->Buffer[us.Length / sizeof(WCHAR)] = UNICODE_NULL;

        return string;
    }
    else
    {
        return NULL;
    }
}

PPH_STRING PhGetTokenUserString(
    _In_ HANDLE TokenHandle, 
    _In_ BOOLEAN IncludeDomain
    )
{
    PPH_STRING tokenUserString = NULL;
    PTOKEN_USER tokenUser;

    if (NT_SUCCESS(PhGetTokenUser(TokenHandle, &tokenUser)))
    {
        tokenUserString = PhGetSidFullName(tokenUser->User.Sid, IncludeDomain, NULL);
        PhFree(tokenUser);
    }

    return tokenUserString;
}

typedef struct _PH_CAPABILITY_ENTRY
{
    PPH_STRING Name;
    PSID CapabilityGroupSid;
    PSID CapabilitySid;
} PH_CAPABILITY_ENTRY, *PPH_CAPABILITY_ENTRY;

PH_ARRAY PhpSidCapArrayList;

VOID PhInitializeCapabilitySidCache(
    VOID
    )
{
    NTSTATUS (NTAPI *RtlDeriveCapabilitySidsFromName_I)(
        _Inout_ PUNICODE_STRING UnicodeString,
        _Out_ PSID CapabilityGroupSid,
        _Out_ PSID CapabilitySid
        );
    PPH_STRING applicationDirectory;
    PPH_STRING capabilityListFileName;
    PPH_STRING capabilityListString;
    PH_STRINGREF namePart;
    PH_STRINGREF remainingPart;

    if (!(RtlDeriveCapabilitySidsFromName_I = PhGetDllProcedureAddress(L"ntdll.dll", "RtlDeriveCapabilitySidsFromName", 0)))
        return;

    if (applicationDirectory = PhGetApplicationDirectory())
    {
        capabilityListFileName = PhConcatStringRefZ(&applicationDirectory->sr, L"capslist.txt");
        PhDereferenceObject(applicationDirectory);

        capabilityListString = PhFileReadAllText(capabilityListFileName->Buffer);
        PhDereferenceObject(capabilityListFileName);      
    }

    if (PhIsNullOrEmptyString(capabilityListString))
        return;

    PhInitializeArray(&PhpSidCapArrayList, sizeof(PH_CAPABILITY_ENTRY), 800);
    remainingPart = capabilityListString->sr;

    while (remainingPart.Length != 0)
    {
        PhSplitStringRefAtChar(&remainingPart, '\n', &namePart, &remainingPart);

        if (namePart.Length != 0)
        {
            BYTE capabilityGroupSidBuffer[SECURITY_MAX_SID_SIZE];
            BYTE capabilitySidBuffer[SECURITY_MAX_SID_SIZE];
            PSID capabilityGroupSid = (PSID)capabilityGroupSidBuffer;
            PSID capabilitySid = (PSID)capabilitySidBuffer;
            UNICODE_STRING capabilityNameUs;

            if (PhEndsWithStringRef2(&namePart, L"\r", FALSE))
                namePart.Length -= sizeof(WCHAR);

            if (!PhStringRefToUnicodeString(&namePart, &capabilityNameUs))
                continue;

            if (NT_SUCCESS(RtlDeriveCapabilitySidsFromName_I(
                &capabilityNameUs,
                capabilityGroupSid,
                capabilitySid
                )))
            {
                PH_CAPABILITY_ENTRY entry;

                entry.Name = PhCreateStringFromUnicodeString(&capabilityNameUs);
                entry.CapabilityGroupSid = PhAllocateCopy(capabilityGroupSid, RtlLengthSid(capabilityGroupSid));
                entry.CapabilitySid = PhAllocateCopy(capabilitySid, RtlLengthSid(capabilitySid));

                PhAddItemArray(&PhpSidCapArrayList, &entry);
            }
        }
    }

    PhDereferenceObject(capabilityListString);
}

PPH_STRING PhGetCapabilitySidName(
    _In_ PSID CapabilitySid
    )
{
    static PH_INITONCE initOnce = PH_INITONCE_INIT;
    PPH_CAPABILITY_ENTRY entry;
    ULONG i;

    if (WindowsVersion < WINDOWS_8)
        return NULL;

    if (PhBeginInitOnce(&initOnce))
    {
        PhInitializeCapabilitySidCache();
        PhEndInitOnce(&initOnce);
    }

    for (i = 0; i < PhpSidCapArrayList.Count; i++)
    {
        entry = PhItemArray(&PhpSidCapArrayList, i);

        if (RtlEqualSid(entry->CapabilitySid, CapabilitySid))
        {
            return PhReferenceObject(entry->Name);
        }

        if (RtlEqualSid(entry->CapabilityGroupSid, CapabilitySid))
        {
            return PhReferenceObject(entry->Name);
        }
    }

    return NULL;
}

typedef struct _PH_CAPABILITY_GUID_ENTRY
{
    PPH_STRING Name;
    PPH_STRING CapabilityGuid;
} PH_CAPABILITY_GUID_ENTRY, *PPH_CAPABILITY_GUID_ENTRY;

PH_ARRAY PhpCapGuidArrayList;

BOOLEAN NTAPI PhpTokenEnumerateKeyCallback(
    _In_ PKEY_BASIC_INFORMATION Information,
    _In_opt_ PVOID Context
    )
{
    PhAddItemList(Context, PhCreateStringEx(Information->Name, Information->NameLength));
    return TRUE;
}

VOID PhInitializeCapabilityGuidCache(
    VOID
    )
{
    static PH_STRINGREF accessManagerKeyPath = PH_STRINGREF_INIT(L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\Capabilities");
    static PH_STRINGREF deviceAccessKeyPath = PH_STRINGREF_INIT(L"Software\\Microsoft\\Windows\\CurrentVersion\\DeviceAccess\\CapabilityMappings");
    HANDLE keyHandle;
    ULONG i;

    PhInitializeArray(&PhpCapGuidArrayList, sizeof(PH_CAPABILITY_GUID_ENTRY), 100);

    if (NT_SUCCESS(PhOpenKey(
        &keyHandle,
        KEY_READ,
        PH_KEY_LOCAL_MACHINE,
        &accessManagerKeyPath,
        0
        )))
    {
        PPH_LIST capabilityNameList;

        capabilityNameList = PhCreateList(1);
        PhEnumerateKey(keyHandle, PhpTokenEnumerateKeyCallback, capabilityNameList);

        for (i = 0; i < capabilityNameList->Count; i++)
        {
            HANDLE subKeyHandle;
            PPH_STRING subKeyName;
            PPH_STRING guidString;

            subKeyName = capabilityNameList->Items[i];

            if (NT_SUCCESS(PhOpenKey(
                &subKeyHandle,
                KEY_READ,
                keyHandle,
                &subKeyName->sr,
                0
                )))
            {
                if (guidString = PhQueryRegistryString(subKeyHandle, L"LegacyInterfaceClassGuid"))
                {
                    PH_CAPABILITY_GUID_ENTRY entry;

                    PhSetReference(&entry.Name, subKeyName);
                    PhSetReference(&entry.CapabilityGuid, guidString);
                    PhAddItemArray(&PhpCapGuidArrayList, &entry);

                    PhDereferenceObject(guidString);
                }

                NtClose(subKeyHandle);
            }
        }

        PhDereferenceObjects(capabilityNameList->Items, capabilityNameList->Count);
        PhDereferenceObject(capabilityNameList);

        NtClose(keyHandle);
    }

    if (NT_SUCCESS(PhOpenKey(
        &keyHandle,
        KEY_READ,
        PH_KEY_LOCAL_MACHINE,
        &deviceAccessKeyPath,
        0
        )))
    {
        PPH_LIST capabilityNameList;

        capabilityNameList = PhCreateList(1);
        PhEnumerateKey(keyHandle, PhpTokenEnumerateKeyCallback, capabilityNameList);

        for (i = 0; i < capabilityNameList->Count; i++)
        {
            HANDLE subKeyHandle;
            PPH_STRING subKeyName;

            subKeyName = capabilityNameList->Items[i];

            if (NT_SUCCESS(PhOpenKey(
                &subKeyHandle,
                KEY_READ,
                keyHandle,
                &subKeyName->sr,
                0
                )))
            {
                PPH_LIST capabilityGuidList;
                ULONG ii;

                capabilityGuidList = PhCreateList(1);
                PhEnumerateKey(subKeyHandle, PhpTokenEnumerateKeyCallback, capabilityGuidList);

                for (ii = 0; ii < capabilityGuidList->Count; ii++)
                {
                    PPH_STRING guidString;
                    PH_CAPABILITY_GUID_ENTRY entry;

                    guidString = capabilityGuidList->Items[ii];

                    PhSetReference(&entry.Name, subKeyName);
                    PhSetReference(&entry.CapabilityGuid, guidString);

                    PhAddItemArray(&PhpCapGuidArrayList, &entry);
                }

                PhDereferenceObjects(capabilityGuidList->Items, capabilityGuidList->Count);
                PhDereferenceObject(capabilityGuidList);

                NtClose(subKeyHandle);
            }
        }

        PhDereferenceObjects(capabilityNameList->Items, capabilityNameList->Count);
        PhDereferenceObject(capabilityNameList);

        NtClose(keyHandle);
    }
}

PPH_STRING PhGetCapabilityGuidName(
    _In_ PPH_STRING GuidString
    )
{
    static PH_INITONCE initOnce = PH_INITONCE_INIT;
    PPH_CAPABILITY_GUID_ENTRY entry;
    ULONG i;

    if (WindowsVersion < WINDOWS_8)
        return NULL;

    if (PhBeginInitOnce(&initOnce))
    {
        PhInitializeCapabilityGuidCache();
        PhEndInitOnce(&initOnce);
    }

    for (i = 0; i < PhpCapGuidArrayList.Count; i++)
    {
        entry = PhItemArray(&PhpCapGuidArrayList, i);

        if (PhEqualString(entry->CapabilityGuid, GuidString, TRUE))
        {
            return PhReferenceObject(entry->Name);
        }
    }

    return NULL;
}
