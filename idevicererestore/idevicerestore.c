/*
 * idevicerestore.c
 * Restore device firmware and filesystem
 *
 * Copyright (c) 2010-2015 Martin Szulecki. All Rights Reserved.
 * Copyright (c) 2012-2015 Nikias Bassen. All Rights Reserved.
 * Copyright (c) 2010 Joshua Hill. All Rights Reserved.
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <getopt.h>
#include <plist/plist.h>
#include <zlib.h>
#include <libgen.h>

#include <curl/curl.h>

#include "dfu.h"
#include "tss.h"
#include "img3.h"
#include "img4.h"
#include "ipsw.h"
#include "common.h"
#include "normal.h"
#include "restore.h"
#include "download.h"
#include "recovery.h"
#include "idevicerestore.h"
#include "partial.h"

#include "locking.h"

#define VERSION_XML "version.xml"

#include <openssl/sha.h>

#ifndef IDEVICERESTORE_NOMAIN
static struct option longopts[] = {
    { "debug",   no_argument,       NULL, 'd' },
    { "help",    no_argument,       NULL, 'h' },
    { "rerestore",    no_argument,      NULL, 'r' },
    { NULL, 0, NULL, 0 }
};

void usage(int argc, char* argv[]) {
    char* name = strrchr(argv[0], '/');
    printf("Usage: %s [OPTIONS] IPSW\n\n", (name ? name + 1 : argv[0]));
    printf("  -r, --rerestore\ttake advantage of the 9.x 32 bit re-restore bug\n");
    printf("  -d, --debug\t\tprint debug information\n");
    printf("\n");
    printf("Homepage: https://downgrade.party\n");
    printf("Based on idevicerestore by libimobiledevice.\n");
}
#endif


static int idevicerestore_keep_pers = 0;

static int load_version_data(struct idevicerestore_client_t* client)
{
    if (!client) {
        return -1;
    }
    
    struct stat fst;
    int cached = 0;
    
    char version_xml[1024];
    
    if (client->cache_dir) {
        if (stat(client->cache_dir, &fst) < 0) {
            mkdir_with_parents(client->cache_dir, 0755);
        }
        strcpy(version_xml, client->cache_dir);
        strcat(version_xml, "/");
        strcat(version_xml, VERSION_XML);
    } else {
        strcpy(version_xml, VERSION_XML);
    }
    
    if ((stat(version_xml, &fst) < 0) || ((time(NULL)-86400) > fst.st_mtime)) {
        char version_xml_tmp[1024];
        strcpy(version_xml_tmp, version_xml);
        strcat(version_xml_tmp, ".tmp");
        
        if (download_to_file("http://itunes.apple.com/check/version",  version_xml_tmp, 0) == 0) {
            remove(version_xml);
            if (rename(version_xml_tmp, version_xml) < 0) {
                error("ERROR: Could not update '%s'\n", version_xml);
            } else {
                info("NOTE: Updated version data.\n");
            }
        }
    } else {
        cached = 1;
    }
    
    char *verbuf = NULL;
    size_t verlen = 0;
    read_file(version_xml, (void**)&verbuf, &verlen);
    
    if (!verbuf) {
        error("ERROR: Could not load '%s'\n", version_xml);
        return -1;
    }
    
    client->version_data = NULL;
    plist_from_xml(verbuf, verlen, &client->version_data);
    free(verbuf);
    
    if (!client->version_data) {
        remove(version_xml);
        error("ERROR: Cannot parse plist data from '%s'.\n", version_xml);
        return -1;
    }
    
    if (cached) {
        info("NOTE: using cached version data\n");
    }
    
    return 0;
}

int idevicerestore_start(struct idevicerestore_client_t* client)
{
    int tss_enabled = 0;
    int result = 0;
    
    if (!client) {
        return -1;
    }
    
    if (client->flags & FLAG_RERESTORE) {
        if (!(client->flags & FLAG_ERASE) && !(client->flags & FLAG_UPDATE)) {
            
            /* Set FLAG_ERASE for now, code later on handles switching to FLAG_UPDATE if needed. */
            
            client->flags |= FLAG_ERASE;
            
#if 0
            error("ERROR: FLAG_RERESTORE must be used with either FLAG_ERASE or FLAG_UPDATE\n");
            return -1;
#endif
            
        }
    }
    
    if ((client->flags & FLAG_LATEST) && (client->flags & FLAG_CUSTOM)) {
        error("ERROR: FLAG_LATEST cannot be used with FLAG_CUSTOM.\n");
        return -1;
    }
    
    if (!client->ipsw && !(client->flags & FLAG_LATEST)) {
        error("ERROR: no ipsw file given\n");
        return -1;
    }
    
    if (client->flags & FLAG_DEBUG) {
        idevice_set_debug_level(1);
        irecv_set_debug_level(1);
        idevicerestore_debug = 1;
    }
    
    idevicerestore_progress(client, RESTORE_STEP_DETECT, 0.0);
    
    // update version data (from cache, or apple if too old)
    load_version_data(client);
    
    // check which mode the device is currently in so we know where to start
    
    
    
    /* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
    if (check_mode(client) < 0) {
        error("ERROR: Unable to discover device mode. Please make sure a device is attached.\n");
        return -1;
    }
    idevicerestore_progress(client, RESTORE_STEP_DETECT, 0.1);
    
    info("Found device in %s mode\n", client->mode->string);
    
    
    if (client->mode->index == MODE_WTF) {
        unsigned int cpid = 0;
        
        if (dfu_client_new(client) != 0) {
            error("ERROR: Could not open device in WTF mode\n");
            return -1;
        }
        if ((dfu_get_cpid(client, &cpid) < 0) || (cpid == 0)) {
            error("ERROR: Could not get CPID for WTF mode device\n");
            dfu_client_free(client);
            return -1;
        }
        
        char wtfname[256];
        sprintf(wtfname, "Firmware/dfu/WTF.s5l%04xxall.RELEASE.dfu", cpid);
        unsigned char* wtftmp = NULL;
        unsigned int wtfsize = 0;
        
        // Prefer to get WTF file from the restore IPSW
        ipsw_extract_to_memory(client->ipsw, wtfname, &wtftmp, &wtfsize);
        if (!wtftmp) {
            // Download WTF IPSW
            char* s_wtfurl = NULL;
            plist_t wtfurl = plist_access_path(client->version_data, 7, "MobileDeviceSoftwareVersionsByVersion", "5", "RecoverySoftwareVersions", "WTF", "304218112", "5", "FirmwareURL");
            if (wtfurl && (plist_get_node_type(wtfurl) == PLIST_STRING)) {
                plist_get_string_val(wtfurl, &s_wtfurl);
            }
            if (!s_wtfurl) {
                info("Using hardcoded x12220000_5_Recovery.ipsw URL\n");
                s_wtfurl = strdup("http://appldnld.apple.com.edgesuite.net/content.info.apple.com/iPhone/061-6618.20090617.Xse7Y/x12220000_5_Recovery.ipsw");
            }
            
            // make a local file name
            char* fnpart = strrchr(s_wtfurl, '/');
            if (!fnpart) {
                fnpart = (char*)"x12220000_5_Recovery.ipsw";
            }
            else {
                fnpart++;
            }
            struct stat fst;
            char wtfipsw[1024];
            if (client->cache_dir) {
                if (stat(client->cache_dir, &fst) < 0) {
                    mkdir_with_parents(client->cache_dir, 0755);
                }
                strcpy(wtfipsw, client->cache_dir);
                strcat(wtfipsw, "/");
                strcat(wtfipsw, fnpart);
            }
            else {
                strcpy(wtfipsw, fnpart);
            }
            if (stat(wtfipsw, &fst) != 0) {
                download_to_file(s_wtfurl, wtfipsw, 0);
            }
            
            ipsw_extract_to_memory(wtfipsw, wtfname, &wtftmp, &wtfsize);
            if (!wtftmp) {
                error("ERROR: Could not extract WTF\n");
            }
        }
        
        if (wtftmp) {
            if (dfu_send_buffer(client, wtftmp, wtfsize) != 0) {
                error("ERROR: Could not send WTF...\n");
            }
        }
        dfu_client_free(client);
        
        sleep(1);
        
        free(wtftmp);
        client->mode = &idevicerestore_modes[MODE_DFU];
    }
    
    // discover the device type
    if (check_hardware_model(client) == NULL || client->device == NULL) {
        error("ERROR: Unable to discover device model\n");
        return -1;
    }
    idevicerestore_progress(client, RESTORE_STEP_DETECT, 0.2);
    info("Identified device as %s, %s\n", client->device->hardware_model, client->device->product_type);
    
    if (client->flags & FLAG_LATEST) {
        char* ipsw = NULL;
        int res = ipsw_download_latest_fw(client->version_data, client->device->product_type, client->cache_dir, &ipsw);
        if (res != 0) {
            if (ipsw) {
                free(ipsw);
            }
            return res;
        }
        else {
            client->ipsw = ipsw;
        }
    }
    idevicerestore_progress(client, RESTORE_STEP_DETECT, 0.6);
    
    if (client->flags & FLAG_NOACTION) {
        return 0;
    }
    
    if (client->mode->index == MODE_RESTORE) {
        if (restore_reboot(client) < 0) {
            error("ERROR: Unable to exit restore mode\n");
            return -2;
        }
        
        // we need to refresh the current mode again
        if (check_mode(client) < 0) {
            error("ERROR: Unable to discover device mode. Please make sure a device is attached.\n");
            return -1;
        }
        info("Found device in %s mode\n", client->mode->string);
    }
    
    // verify if ipsw file exists
    if (access(client->ipsw, F_OK) < 0) {
        error("ERROR: Firmware file %s does not exist.\n", client->ipsw);
        return -1;
    }
    
    // extract buildmanifest
    plist_t buildmanifest = NULL;
    info("Extracting BuildManifest from IPSW\n");
    if (ipsw_extract_build_manifest(client->ipsw, &buildmanifest, &tss_enabled) < 0) {
        error("ERROR: Unable to extract BuildManifest from %s. Firmware file might be corrupt.\n", client->ipsw);
        return -1;
    }
    
    idevicerestore_progress(client, RESTORE_STEP_DETECT, 0.8);
    
    
    /* check if device type is supported by the given build manifest */
    if (build_manifest_check_compatibility(buildmanifest, client->device->product_type) < 0) {
        error("ERROR: Could not make sure this firmware is suitable for the current device. Refusing to continue.\n");
        return -1;
    }
    
    /* print iOS information from the manifest */
    build_manifest_get_version_information(buildmanifest, client);
    
    info("Product Version: %s\n", client->version);
    info("Product Build: %s Major: %d\n", client->build, client->build_major);
    
    client->image4supported = is_image4_supported(client);
    debug("Device supports Image4: %s\n", (client->image4supported) ? "true" : "false");
    
    if (client->image4supported) {
        error("This copy of iDeviceReRestore does not support Image4 devices. Use iDeviceRestore instead (https://github.com/libimobiledevice/idevicerestore)\n");
        return -1;
    }
    
    
    client->tss = NULL;
    plist_t build_identity = NULL;
    
    if (client->flags & FLAG_ERASE) {
        build_identity = build_manifest_get_build_identity_for_model_with_restore_behavior(buildmanifest, client->device->hardware_model, "Erase");
        if (build_identity == NULL) {
            error("ERROR: Unable to find any build identities\n");
            plist_free(buildmanifest);
            return -1;
        }
    }
    else if (client->flags & FLAG_UPDATE) {
        build_identity = build_manifest_get_build_identity_for_model_with_restore_behavior(buildmanifest, client->device->hardware_model, "Update");
        if (!build_identity) {
            build_identity = build_manifest_get_build_identity_for_model(buildmanifest, client->device->hardware_model);
        }
    }
    else {
        error("No install option chosen.\n");
        exit(1);
    }
    
    plist_t buildmanifest2 = NULL;
    plist_t build_identity2 = NULL;
    
    idevicerestore_progress(client, RESTORE_STEP_PREPARE, 0.0);
    /* retrieve shsh blobs if required */
    debug("Getting device's ECID for TSS request\n");
    /* fetch the device's ECID for the TSS request */
    if (get_ecid(client, &client->ecid) < 0) {
        error("ERROR: Unable to find device ECID\n");
        return -1;
    }
    info("Found ECID " FMT_qu "\n", (long long unsigned int)client->ecid);
    
    if (client->build_major > 8) {
        unsigned char* nonce = NULL;
        int nonce_size = 0;
        if (get_ap_nonce(client, &nonce, &nonce_size) < 0) {
            /* the first nonce request with older firmware releases can fail and it's OK */
            info("NOTE: Unable to get nonce from device\n");
        }
        
        if (!client->nonce || (nonce_size != client->nonce_size) || (memcmp(nonce, client->nonce, nonce_size) != 0)) {
            if (client->nonce) {
                free(client->nonce);
            }
            client->nonce = nonce;
            client->nonce_size = nonce_size;
        }
        else {
            free(nonce);
        }
    }
    
    if (get_tss_response(client, build_identity, &client->tss) < 0) {
        error("ERROR: Unable to get SHSH blobs for this device\n");
        return -1;
    }
    
    if (client->flags & FLAG_SHSHONLY) {
        if (!client->tss) {
            error("ERROR: could not fetch TSS record\n");
            plist_free(buildmanifest);
            return -1;
        }
        else {
            char *bin = NULL;
            uint32_t blen = 0;
            plist_to_bin(client->tss, &bin, &blen);
            if (bin) {
                char zfn[1024];
                if (client->cache_dir) {
                    strcpy(zfn, client->cache_dir);
                    strcat(zfn, "/shsh");
                }
                else {
                    strcpy(zfn, "shsh");
                }
                mkdir_with_parents(zfn, 0755);
                sprintf(zfn + strlen(zfn), "/" FMT_qu "-%s-%s-%s.shsh", (long long int)client->ecid, client->device->product_type, client->version, client->build);
                struct stat fst;
                if (stat(zfn, &fst) != 0) {
                    gzFile zf = gzopen(zfn, "wb");
                    gzwrite(zf, bin, blen);
                    gzclose(zf);
                    info("SHSH saved to '%s'\n", zfn);
                }
                else {
                    info("SHSH '%s' already present.\n", zfn);
                }
                free(bin);
            }
            else {
                error("ERROR: could not get TSS record data\n");
            }
            plist_free(client->tss);
            plist_free(buildmanifest);
            return 0;
        }
    }
    
    /* For a re-restore, check the APTicket for a hash of the RestoreRamDisk in the BuildManifest,
     * try to automatically detect if it contains an Erase or Update ramdisk hash, then
     * update the client flags if required.
     */
    if (tss_enabled && (client->flags & FLAG_RERESTORE)) {
        
        unsigned int ticketSize = 0;
        unsigned char *ticketData = 0;
        
        int ret = 0;
        
        /* Try to get the APTicket from the TSS response */
        ret = tss_response_get_ap_ticket(client->tss, &ticketData, &ticketSize);
        
        /* Check if an error was returned, or if no data was returned */
        if (!(ticketSize && ticketData) || ret) {
            printf("Error getting APTicket from TSS response\n");
            goto rdcheckdone;
        }
        
        int tries = 0;
        
    retry:;
        
        char *component = "RestoreRamDisk";
        char *path = 0;
        
        /* Try to get the path of the RestoreRamDisk for the current build identity */
        if (build_identity_get_component_path(build_identity, component, &path) < 0) {
            error("ERROR: Unable to get path for component '%s'\n", component);
            
            if (path) {
                free(path);
            }
            
            free(ticketData);
            goto rdcheckdone;
        }
        
        unsigned char *ramdiskData = 0;
        unsigned int ramdiskSize = 0;
        
        /* Try to get a buffer with the RestoreRamDisk */
        ret = extract_component(client->ipsw, path, &ramdiskData, &ramdiskSize);
        
        free(path);
        
        if (ret < 0 || !(ramdiskSize && ramdiskData)) {
            error("ERROR: Unable to extract component: %s\n", component);
            free(ticketData);
            goto rdcheckdone;
        }
        
        if (ramdiskSize < 0x14) {
            debug("Ramdisk data was not large enough to be an Image3\n");
            free(ramdiskData);
            free(ticketData);
            goto rdcheckdone;
        }
        
        /* If an unsigned RestoreRamDisk image is encountered, this is probably a custom restore. Move on from here. */
        if (*(uint32_t*)(void*)(ramdiskData+0xC) == 0x0) {
            free(ticketData);
            free(ramdiskData);
            client->flags |= FLAG_CUSTOM;
            goto rdcheckdone;
        }
        
        /* Create a buffer for the RestoreRamDisk digest */
        void *hashBuf = malloc(0x14);
        
        if (!hashBuf) {
            goto rdcheckdone;
        }
        
        bzero(hashBuf, 0x14);
        
        /* Hash the signed Image3 contents */
        SHA1(ramdiskData+0xC, (ramdiskSize-0xC), hashBuf);
        
        free(ramdiskData);
        
        int foundHash = 0;
        
        /* Search the ticket for the computed RestoreRamDisk digest */
        for (int i=0; i < (ticketSize-0x14); i++) {
            if (!memcmp(ticketData+i, hashBuf, 0x14)) {
                debug("Found ramdisk hash in ticket\n");
                foundHash = 1;
                break;
            }
        }
        
        /* Free the hash */
        free(hashBuf);
        
        /* If the RestoreRamDisk digest hash wasn't found in the APTicket, change the build identity and try again. */
        if (!foundHash) {
            
            /* Only change build identity if we haven't already */
            if (!tries) {
                if (client->flags & FLAG_ERASE) {
                    /* Remove FLAG_ERASE */
                    client->flags &= ~FLAG_ERASE;
                    
                    /* Set build_identity to Update */
                    build_identity = build_manifest_get_build_identity_for_model_with_restore_behavior(buildmanifest, client->device->hardware_model, "Update");
                    
                    /* If build_identity comes back NULL, there might not be an Update identity in the manifest. */
                    if (!build_identity) {
                        /* Set FLAG_ERASE */
                        client->flags |= FLAG_ERASE;
                        
                        /* Switch build identity back to Erase */
                        build_identity = build_manifest_get_build_identity_for_model_with_restore_behavior(buildmanifest, client->device->hardware_model, "Erase");
                        
                        /* Free the ticket data */
                        free(ticketData);
                        
                        /* Continue from here */
                        goto rdcheckdone;
                    }
                }
                else {
                    /* Set FLAG_ERASE */
                    client->flags |= FLAG_ERASE;
                    
                    /* Change build_identity to Erase */
                    build_identity = build_manifest_get_build_identity_for_model_with_restore_behavior(buildmanifest, client->device->hardware_model, "Erase");
                }
            }
            
            /* Didn't find the hash in the attempted build_identities, set to Erase and continue the restore. */
            else {
                /* Set FLAG_ERASE */
                client->flags |= FLAG_ERASE;
                
                /* Change build_identity to Erase */
                build_identity = build_manifest_get_build_identity_for_model_with_restore_behavior(buildmanifest, client->device->hardware_model, "Erase");
                
                /* Free the ticket data */
                free(ticketData);
                
                /* We can probably safely assume here that this is a custom restore if we haven't found the RestoreRamDisk hashes in the ticket */
                if (!(client->flags & FLAG_CUSTOM)) {
                    client->flags |= FLAG_CUSTOM;
                }
                
                /* Continue from here */
                goto rdcheckdone;
            }
            
            debug("Didn't find ramdisk hash in ticket, checking for other ramdisk hash\n");
            
            /* Increment the tries counter */
            tries+=1;
            
            /* Retry */
            goto retry;
        }
        
    }
    
rdcheckdone:
    
    /* The build_identity may have been changed, print information about it here */
    build_identity_print_information(build_identity);
    
    /* Verify if we have tss records if required */
    if ((tss_enabled) && (client->tss == NULL)) {
        error("ERROR: Unable to proceed without a TSS record.\n");
        plist_free(buildmanifest);
        return -1;
    }
    
    if ((tss_enabled) && client->tss) {
        /* fix empty dicts */
        fixup_tss(client->tss);
    }
    idevicerestore_progress(client, RESTORE_STEP_PREPARE, 0.1);
    
    // Get filesystem name from build identity
    char* fsname = NULL;
    if (build_identity_get_component_path(build_identity, "OS", &fsname) < 0) {
        error("ERROR: Unable get path for filesystem component\n");
        return -1;
    }
    
    // check if we already have an extracted filesystem
    int delete_fs = 0;
    char* filesystem = NULL;
    struct stat st;
    memset(&st, '\0', sizeof(struct stat));
    char tmpf[1024];
    if (client->cache_dir) {
        if (stat(client->cache_dir, &st) < 0) {
            mkdir_with_parents(client->cache_dir, 0755);
        }
        strcpy(tmpf, client->cache_dir);
        strcat(tmpf, "/");
        char *ipswtmp = strdup(client->ipsw);
        strcat(tmpf, basename(ipswtmp));
        free(ipswtmp);
    }
    else {
        strcpy(tmpf, client->ipsw);
    }
    char* p = strrchr((const char*)tmpf, '.');
    if (p) {
        *p = '\0';
    }
    
    if (stat(tmpf, &st) < 0) {
        mkdir(tmpf, 0755);
    }
    strcat(tmpf, "/");
    strcat(tmpf, fsname);
    
    memset(&st, '\0', sizeof(struct stat));
    if (stat(tmpf, &st) == 0) {
        off_t fssize = 0;
        ipsw_get_file_size(client->ipsw, fsname, &fssize);
        if ((fssize > 0) && (st.st_size == fssize)) {
            info("Using cached filesystem from '%s'\n", tmpf);
            filesystem = strdup(tmpf);
        }
    }
    
    if (!filesystem) {
        char extfn[1024];
        strcpy(extfn, tmpf);
        strcat(extfn, ".extract");
        char lockfn[1024];
        strcpy(lockfn, tmpf);
        strcat(lockfn, ".lock");
        lock_info_t li;
        
        lock_file(lockfn, &li);
        FILE* extf = NULL;
        if (access(extfn, F_OK) != 0) {
            extf = fopen(extfn, "w");
        }
        unlock_file(&li);
        if (!extf) {
            // use temp filename
            filesystem = tempnam(NULL, "ipsw_");
            if (!filesystem) {
                error("WARNING: Could not get temporary filename, using '%s' in current directory\n", fsname);
                filesystem = strdup(fsname);
            }
            delete_fs = 1;
        }
        else {
            // use <fsname>.extract as filename
            filesystem = strdup(extfn);
            fclose(extf);
        }
        remove(lockfn);
        
        // Extract filesystem from IPSW
        info("Extracting filesystem from IPSW\n");
        if (ipsw_extract_to_file_with_progress(client->ipsw, fsname, filesystem, 1) < 0) {
            error("ERROR: Unable to extract filesystem from IPSW\n");
            if (client->tss)
                plist_free(client->tss);
            plist_free(buildmanifest);
            return -1;
        }
        
        if (strstr(filesystem, ".extract")) {
            // rename <fsname>.extract to <fsname>
            remove(tmpf);
            rename(filesystem, tmpf);
            free(filesystem);
            filesystem = strdup(tmpf);
        }
    }
    
    
    // if the device is in normal mode, place device into recovery mode
    if (client->mode->index == MODE_NORMAL) {
        info("Entering recovery mode...\n");
        if (normal_enter_recovery(client) < 0) {
            error("ERROR: Unable to place device into recovery mode from %s mode\n", client->mode->string);
            if (client->tss)
                plist_free(client->tss);
            plist_free(buildmanifest);
            return -5;
        }
    }
    
    idevicerestore_progress(client, RESTORE_STEP_PREPARE, 0.3);
    
    // if the device is in DFU mode, place device into recovery mode
    if (client->mode->index == MODE_DFU) {
        dfu_client_free(client);
        recovery_client_free(client);
        if (dfu_enter_recovery(client, build_identity) < 0) {
            error("ERROR: Unable to place device into recovery mode from %s mode\n", client->mode->string);
            plist_free(buildmanifest);
            if (client->tss)
                plist_free(client->tss);
            if (delete_fs && filesystem)
                unlink(filesystem);
            return -2;
        }
    }
    else {
        if (client->build_major > 8) {
            if (client->image4supported) {
                error("This copy of iDeviceReRestore does not support Image4 devices. Use iDeviceRestore instead (https://github.com/libimobiledevice/idevicerestore)\n");
                return -1;
            }
            else {
                /* send ApTicket */
                if (recovery_send_ticket(client) < 0) {
                    error("WARNING: Unable to send APTicket\n");
                }
            }
        }
        
        /* now we load the iBEC */
        if (recovery_send_ibec(client, build_identity) < 0) {
            error("ERROR: Unable to send iBEC\n");
            if (delete_fs && filesystem)
                unlink(filesystem);
            return -2;
        }
        
        recovery_client_free(client);
        
        /* Wait 2s after attempting to boot the image */
        sleep(2);
        
        int mode = 0;
        
        /* Try checking for the device's mode for about 10 seconds until it's in recovery again */
        for (int i=0; i < 20; i++) {
            
            /* Get the current mode */
            mode = check_mode(client);
            
            /* If mode came back NULL, wait 0.5s and try again */
            if (!mode) {
                usleep(500000);
                continue;
            }
            
            /* If the current mode is not recovery, wait 0.5s and try again */
            if (mode != MODE_RECOVERY) {
                usleep(500000);
                continue;
            }
            
            /* Hello recovery */
            
            if (recovery_client_new(client)) {
                error("Failed to connect to device\n");
                return -1;
            }
            
            break;
        }
    }
    
    /* Check the IBFL to see if we've successfully entered iBEC */
    const struct irecv_device_info *device_info = irecv_get_device_info(client->recovery->client);
    
    if (!device_info) {
        error("Couldn't query device info\n");
        return -1;
    }
    
    switch (device_info->ibfl) {
        case 0x03:
        case 0x1B:
            
            if ((client->flags & FLAG_CUSTOM) || !(client->build_major == 9 || client->build_major == 13)) {
                error("Failed to enter iBEC.\n");
            }
            else {
                error("Failed to enter iBEC. Your APTicket might not be usable for re-restoring.\n");
            }
            
            return -1;
            
        case 0x1A:
        case 0x02:
            printf("Successfully entered iBEC\n");
            
        default:
            break;
    }
    
    recovery_client_free(client);
    
    idevicerestore_progress(client, RESTORE_STEP_PREPARE, 0.5);
    
    if (client->flags & FLAG_RERESTORE) {
        char* fwurl = NULL;
        unsigned char isha1[20];
        
        if ((ipsw_get_latest_fw(client->version_data, client->device->product_type, &fwurl, isha1) < 0) || !fwurl) {
            error("ERROR: can't get URL for latest firmware\n");
            return -1;
        }
        
        /* download latest firmware's BuildManifest to grab bbfw path later */
        debug("fwurl: %s\n", fwurl);
        partialzip_download_file(fwurl, "BuildManifest.plist", "BuildManifest_New.plist");
        client->otamanifest = "BuildManifest_New.plist";
        
        FILE *ofp = fopen(client->otamanifest, "rb");
        struct stat *ostat = (struct stat*) malloc(sizeof(struct stat));
        stat(client->otamanifest, ostat);
        char *opl = (char *)malloc(sizeof(char) *(ostat->st_size + 1));
        fread(opl, sizeof(char), ostat->st_size, ofp);
        fclose(ofp);
        
        if (memcmp(opl, "bplist00", 8) == 0)
            plist_from_bin(opl, (uint32_t)ostat->st_size, &buildmanifest2);
        else
            plist_from_xml(opl, (uint32_t)ostat->st_size, &buildmanifest2);
        free(ostat);
        const char *device = client->device->product_type;
        
        int indexCount = -1;
        
        if (!strcmp(device, "iPhone5,2") || !strcmp(device, "iPad3,5"))
            indexCount = 0;
        
        else if (!strcmp(device, "iPhone5,4") || !strcmp(device, "iPad3,6"))
            indexCount = 2;
        
        else if (!strcmp(device, "iPhone5,1") || !strcmp(device, "iPad3,4"))
            indexCount = 4;
        
        else if (!strcmp(device, "iPhone5,3"))
            indexCount = 6;
        
        if (indexCount != -1) {
            if (client->flags & FLAG_UPDATE) {
                indexCount+=1;
            }
        }
        
        plist_t node = NULL;
        char *version = 0;
        char *build = 0;
        node = plist_dict_get_item(buildmanifest2, "ProductVersion");
        plist_get_string_val(node, &version);
        
        node = plist_dict_get_item(buildmanifest2, "ProductBuildVersion");
        plist_get_string_val(node, &build);
        
        unsigned long major = strtoul(build, NULL, 10);
        
        if (major >= 14 && indexCount == -1) {
            error("Error parsing BuildManifest.\n");
            exit(-1);
        }
        else if (major >= 14)
            build_identity2 = build_manifest_get_build_identity(buildmanifest2, indexCount);
        else build_identity2 = build_manifest_get_build_identity(buildmanifest2, 0);
        
        /* if buildmanifest not specified, download the baseband firmware */
        if (!client->manifestPath) {
            char* bbfwpath = NULL;
            printf("Device: %s\n", device);
            plist_t bbfw_path = plist_access_path(build_identity2, 4, "Manifest", "BasebandFirmware", "Info", "Path");
            
            if (!bbfw_path) {
                printf("No BasebandFirmware in manifest\n");
                goto bbdlout;
            }
            
            if (plist_get_node_type(bbfw_path) != PLIST_STRING) {
                goto bbdownload;
            }
            
            plist_get_string_val(bbfw_path, &bbfwpath);
            
            plist_t bbfw_digestIPSW = plist_access_path(build_identity, 2, "Manifest", "BasebandFirmware");
            plist_t bbfw_digestNew = plist_access_path(build_identity2, 2, "Manifest", "BasebandFirmware");
            
            int bbfwIPSWDictCount = plist_dict_get_size(bbfw_digestIPSW);
            int bbfwNewDictCount = plist_dict_get_size(bbfw_digestNew);
            
            if (bbfwIPSWDictCount != bbfwNewDictCount) {
                goto bbdownload;
            }
            
            if (bbfwNewDictCount == 0) {
                goto bbdlout;
            }
            
            plist_dict_iter iter = 0;
            plist_dict_new_iter(bbfw_digestIPSW, &iter);
            
            for (int i=0; i < bbfwNewDictCount; i++) {
                void *item = 0;
                plist_t itemPlistIPSW = 0;
                plist_dict_next_item(bbfw_digestIPSW, iter, (char**)&item, &itemPlistIPSW);
                
                if (!item) {
                    continue;
                }
                
                mode_t currentItemModeIPSW = plist_get_node_type(itemPlistIPSW);
                
                plist_t itemPlistNew = plist_dict_get_item(bbfw_digestNew, item);
                
                if (!itemPlistNew) {
                    debug("Couldn't find %s in new manifest\n", item);
                    free(item);
                    goto bbdownload;
                }
                
                mode_t currentItemModeNew = plist_get_node_type(itemPlistNew);
                
                if (currentItemModeIPSW != currentItemModeNew) {
                    debug("%s does not match the type in new manifest\n", item);
                    free(item);
                    goto bbdownload;
                }
                
                switch (currentItemModeIPSW) {
                    case PLIST_DATA:;
                        
                        void *currentItemIPSW = 0;
                        uint64_t currentItemSizeIPSW = 0;
                        void *currentItemNew = 0;
                        uint64_t currentItemSizeNew = 0;
                        
                        plist_get_data_val(itemPlistIPSW, (char**)&currentItemIPSW, &currentItemSizeIPSW);
                        plist_get_data_val(itemPlistNew, (char**)&currentItemNew, &currentItemSizeNew);
                        
                        if (currentItemSizeIPSW != currentItemSizeNew) {
                            debug("IPSW %s size did not match the new manifest's entry\n", item);
                            free(item);
                            goto bbdownload;
                        }
                        
                        if (!memcmp(currentItemIPSW, currentItemNew, currentItemSizeIPSW)) {
                            debug("IPSW %s matches new manifest item\n", item);
                            free(currentItemIPSW);
                            free(currentItemNew);
                            free(item);
                            continue;
                        }
                        
                        free(currentItemIPSW);
                        free(currentItemNew);
                        free(item);
                        
                        goto bbdownload;
                        
                    case PLIST_UINT:;
                        
                        uint64_t currentUintItemIPSW = 0;
                        uint64_t currentUintItemNew = 0;
                        
                        plist_get_uint_val(itemPlistIPSW, &currentUintItemIPSW);
                        plist_get_uint_val(itemPlistNew, &currentUintItemNew);
                        
                        if (currentUintItemIPSW == currentUintItemNew) {
                            debug("IPSW %s matches new manifest item\n", item);
                            free(item);
                            continue;
                        }
                        
                        printf("IPSW %s did not match manifest item\n", item);
                        
                        free(item);
                        goto bbdownload;
                        
                    case PLIST_DICT:;
                        
                        if (!strcmp(item, "Info")) {
                            free(item);
                            continue;
                        }
                        
                    default:
                        debug("Unhandled item %s\n", item);
                        free(item);
                        goto bbdownload;
                }
                
            }
            
            /* All items in the IPSW bbfw entry match the new manifest, use the bbfw from the ipsw */
            debug("Provided IPSW BasebandFirmware matches the entry found in new manifest, using local file\n");
            
            void *bbfwData = 0;
            size_t bbfwSz = 0;
            
            extract_component(client->ipsw, bbfwpath, (unsigned char**)&bbfwData, (unsigned int*)&bbfwSz);
            
            if (!bbfwSz || !bbfwData) {
                debug("Failed to extract BasebandFirmware from IPSW\n");
                goto bbdownload;
            }
            
            client->basebandPath = "bbfw.tmp";
            
            FILE *bbfwFd = fopen(client->basebandPath, "w");
            fwrite(bbfwData, bbfwSz, 1, bbfwFd);
            fflush(bbfwFd);
            fclose(bbfwFd);
            
            free(bbfwData);
            
            goto bbdlout;
            
        bbdownload:
            
            if (bbfw_path || plist_get_node_type(bbfw_path) != PLIST_STRING) {
                /* download baseband firmware from either 9.3.6 or 10.3.4, depending on the device */
                printf("Downloading baseband firmware.\n");
                plist_get_string_val(bbfw_path, &bbfwpath);
                debug("bbfwpath: %s\n", bbfwpath);
                partialzip_download_file(fwurl, bbfwpath, "bbfw.tmp");
                client->basebandPath = "bbfw.tmp";
            }
        }
    }
    
bbdlout:
    
    if (!client->image4supported && (client->build_major > 8)) {
        // we need another tss request with nonce.
        unsigned char* nonce = NULL;
        int nonce_size = 0;
        int nonce_changed = 0;
        if (get_ap_nonce(client, &nonce, &nonce_size) < 0) {
            error("ERROR: Unable to get nonce from device!\n");
            recovery_send_reset(client);
            if (delete_fs && filesystem)
                unlink(filesystem);
            return -2;
        }
        
        if (!client->nonce || (nonce_size != client->nonce_size) || (memcmp(nonce, client->nonce, nonce_size) != 0)) {
            nonce_changed = 1;
            if (client->nonce) {
                free(client->nonce);
            }
            client->nonce = nonce;
            client->nonce_size = nonce_size;
        } else {
            free(nonce);
        }
        
        if (nonce_changed) {
            // Welcome iOS5. We have to re-request the TSS with our nonce.
            plist_free(client->tss);
            if (get_tss_response(client, build_identity, &client->tss) < 0) {
                error("ERROR: Unable to get SHSH blobs for this device\n");
                if (delete_fs && filesystem)
                    unlink(filesystem);
                return -1;
            }
            if (!client->tss) {
                error("ERROR: can't continue without TSS\n");
                if (delete_fs && filesystem)
                    unlink(filesystem);
                return -1;
            }
            fixup_tss(client->tss);
        }
    }
    idevicerestore_progress(client, RESTORE_STEP_PREPARE, 0.7);
    
    // now finally do the magic to put the device into restore mode
    if (client->mode->index == MODE_RECOVERY) {
        if (client->srnm == NULL) {
            error("ERROR: could not retrieve device serial number. Can't continue.\n");
            if (delete_fs && filesystem)
                unlink(filesystem);
            return -1;
        }
        if (recovery_enter_restore(client, build_identity) < 0) {
            error("ERROR: Unable to place device into restore mode\n");
            plist_free(buildmanifest);
            if (client->tss)
                plist_free(client->tss);
            if (delete_fs && filesystem)
                unlink(filesystem);
            return -2;
        }
        recovery_client_free(client);
    }
    idevicerestore_progress(client, RESTORE_STEP_PREPARE, 0.9);
    
    // device is finally in restore mode, let's do this
    if (client->mode->index == MODE_RESTORE) {
        info("About to restore device... \n");
        result = restore_device(client, build_identity, filesystem);
        if (result < 0) {
            error("ERROR: Unable to restore device\n");
            if (delete_fs && filesystem)
                unlink(filesystem);
            return result;
        }
    }
    
    info("Cleaning up...\n");
    if (delete_fs && filesystem)
        unlink(filesystem);
    
    /* special handling of AppleTVs */
    if (strncmp(client->device->product_type, "AppleTV", 7) == 0) {
        if (recovery_client_new(client) == 0) {
            if (recovery_set_autoboot(client, 1) == 0) {
                recovery_send_reset(client);
            } else {
                error("Setting auto-boot failed?!\n");
            }
        } else {
            error("Could not connect to device in recovery mode.\n");
        }
    }
    
    info("DONE\n");
    
    if (result == 0) {
        idevicerestore_progress(client, RESTORE_NUM_STEPS-1, 1.0);
    }
    
    if (buildmanifest)
        plist_free(buildmanifest);
    
    if (build_identity)
        plist_free(build_identity);
    
    return result;
}

struct idevicerestore_client_t* idevicerestore_client_new(void)
{
    struct idevicerestore_client_t* client = (struct idevicerestore_client_t*) malloc(sizeof(struct idevicerestore_client_t));
    if (client == NULL) {
        error("ERROR: Out of memory\n");
        return NULL;
    }
    memset(client, '\0', sizeof(struct idevicerestore_client_t));
    return client;
}

void idevicerestore_client_free(struct idevicerestore_client_t* client)
{
    if (!client) {
        return;
    }
    
    if (client->tss_url) {
        free(client->tss_url);
    }
    if (client->version_data) {
        plist_free(client->version_data);
    }
    if (client->nonce) {
        free(client->nonce);
    }
    if (client->udid) {
        free(client->udid);
    }
    if (client->srnm) {
        free(client->srnm);
    }
    if (client->ipsw) {
        free(client->ipsw);
    }
    if (client->version) {
        free(client->version);
    }
    if (client->build) {
        free(client->build);
    }
    if (client->restore_boot_args) {
        free(client->restore_boot_args);
    }
    if (client->cache_dir) {
        free(client->cache_dir);
    }
    free(client);
}

void idevicerestore_set_ecid(struct idevicerestore_client_t* client, unsigned long long ecid)
{
    if (!client)
        return;
    client->ecid = ecid;
}

void idevicerestore_set_udid(struct idevicerestore_client_t* client, const char* udid)
{
    if (!client)
        return;
    if (client->udid) {
        free(client->udid);
        client->udid = NULL;
    }
    if (udid) {
        client->udid = strdup(udid);
    }
}

void idevicerestore_set_flags(struct idevicerestore_client_t* client, int flags)
{
    if (!client)
        return;
    client->flags = flags;
}

void idevicerestore_set_ipsw(struct idevicerestore_client_t* client, const char* path)
{
    if (!client)
        return;
    if (client->ipsw) {
        free(client->ipsw);
        client->ipsw = NULL;
    }
    if (path) {
        client->ipsw = strdup(path);
    }
}

void idevicerestore_set_cache_path(struct idevicerestore_client_t* client, const char* path)
{
    if (!client)
        return;
    if (client->cache_dir) {
        free(client->cache_dir);
        client->cache_dir = NULL;
    }
    if (path) {
        client->cache_dir = strdup(path);
    }
}

void idevicerestore_set_progress_callback(struct idevicerestore_client_t* client, idevicerestore_progress_cb_t cbfunc, void* userdata)
{
    if (!client)
        return;
    client->progress_cb = cbfunc;
    client->progress_cb_data = userdata;
}

#ifndef IDEVICERESTORE_NOMAIN
int main(int argc, char* argv[]) {
    int opt = 0;
    int optindex = 0;
    char* ipsw = NULL;
    int result = 0;
    
    struct idevicerestore_client_t* client = idevicerestore_client_new();
    if (client == NULL) {
        error("ERROR: could not create idevicerestore client\n");
        return -1;
    }
    
    while ((opt = getopt_long(argc, argv, "dhcersxtplui:nC:k:", longopts, &optindex)) > 0) {
        switch (opt) {
            case 'h':
                usage(argc, argv);
                return 0;
                
            case 'd':
                client->flags |= FLAG_DEBUG;
                break;
                
            case 'r':
                client->flags |= FLAG_RERESTORE;
                break;
                
            default:
                usage(argc, argv);
                return -1;
        }
    }
    
    if (((argc-optind) == 1) || (client->flags & FLAG_LATEST)) {
        argc -= optind;
        argv += optind;
        
        ipsw = argv[0];
    } else {
        usage(argc, argv);
        return -1;
    }
    
    if ((client->flags & FLAG_LATEST) && (client->flags & FLAG_CUSTOM)) {
        error("ERROR: You can't use --custom and --latest options at the same time.\n");
        return -1;
    }
    
    if (ipsw) {
        client->ipsw = strdup(ipsw);
    }
    
    curl_global_init(CURL_GLOBAL_ALL);
    
    result = idevicerestore_start(client);
    
    idevicerestore_client_free(client);
    
    curl_global_cleanup();
    
    return result;
}
#endif

int check_mode(struct idevicerestore_client_t* client) {
    int mode = MODE_UNKNOWN;
    int dfumode = MODE_UNKNOWN;
    
    if (recovery_check_mode(client) == 0) {
        mode = MODE_RECOVERY;
    }
    
    else if (dfu_check_mode(client, &dfumode) == 0) {
        mode = dfumode;
    }
    
    else if (normal_check_mode(client) == 0) {
        mode = MODE_NORMAL;
    }
    
    else if (restore_check_mode(client) == 0) {
        mode = MODE_RESTORE;
    }
    
    if (mode == MODE_UNKNOWN) {
        client->mode = NULL;
    } else {
        client->mode = &idevicerestore_modes[mode];
    }
    return mode;
}

const char* check_hardware_model(struct idevicerestore_client_t* client) {
    const char* hw_model = NULL;
    int mode = MODE_UNKNOWN;
    
    if (client->mode) {
        mode = client->mode->index;
    }
    
    switch (mode) {
        case MODE_RESTORE:
            hw_model = restore_check_hardware_model(client);
            break;
            
        case MODE_NORMAL:
            hw_model = normal_check_hardware_model(client);
            break;
            
        case MODE_DFU:
        case MODE_RECOVERY:
            hw_model = dfu_check_hardware_model(client);
            break;
        default:
            break;
    }
    
    if (hw_model != NULL) {
        irecv_devices_get_device_by_hardware_model(hw_model, &client->device);
    }
    
    return hw_model;
}

int is_image4_supported(struct idevicerestore_client_t* client)
{
    int res = 0;
    int mode = MODE_UNKNOWN;
    
    if (client->mode) {
        mode = client->mode->index;
    }
    
    switch (mode) {
        case MODE_NORMAL:
            res = normal_is_image4_supported(client);
            break;
        case MODE_DFU:
            res = dfu_is_image4_supported(client);
            break;
        case MODE_RECOVERY:
            res = recovery_is_image4_supported(client);
            break;
        default:
            error("ERROR: Device is in an invalid state\n");
            return 0;
    }
    return res;
}

int get_ecid(struct idevicerestore_client_t* client, uint64_t* ecid) {
    int mode = MODE_UNKNOWN;
    
    if (client->mode) {
        mode = client->mode->index;
    }
    
    switch (mode) {
        case MODE_NORMAL:
            if (normal_get_ecid(client, ecid) < 0) {
                *ecid = 0;
                return -1;
            }
            break;
            
        case MODE_DFU:
            if (dfu_get_ecid(client, ecid) < 0) {
                *ecid = 0;
                return -1;
            }
            break;
            
        case MODE_RECOVERY:
            if (recovery_get_ecid(client, ecid) < 0) {
                *ecid = 0;
                return -1;
            }
            break;
            
        default:
            error("ERROR: Device is in an invalid state\n");
            *ecid = 0;
            return -1;
    }
    
    return 0;
}

int get_ap_nonce(struct idevicerestore_client_t* client, unsigned char** nonce, int* nonce_size) {
    int mode = MODE_UNKNOWN;
    
    *nonce = NULL;
    *nonce_size = 0;
    
    info("Getting ApNonce ");
    
    if (client->mode) {
        mode = client->mode->index;
    }
    
    switch (mode) {
        case MODE_NORMAL:
            info("in normal mode... ");
            if (normal_get_ap_nonce(client, nonce, nonce_size) < 0) {
                info("failed\n");
                return -1;
            }
            break;
        case MODE_DFU:
            info("in dfu mode... ");
            if (dfu_get_ap_nonce(client, nonce, nonce_size) < 0) {
                info("failed\n");
                return -1;
            }
            break;
        case MODE_RECOVERY:
            info("in recovery mode... ");
            if (recovery_get_ap_nonce(client, nonce, nonce_size) < 0) {
                info("failed\n");
                return -1;
            }
            break;
            
        default:
            info("failed\n");
            error("ERROR: Device is in an invalid state\n");
            return -1;
    }
    
    int i = 0;
    for (i = 0; i < *nonce_size; i++) {
        info("%02x", (*nonce)[i]);
    }
    info("\n");
    
    return 0;
}

int get_sep_nonce(struct idevicerestore_client_t* client, unsigned char** nonce, int* nonce_size) {
    int mode = MODE_UNKNOWN;
    
    *nonce = NULL;
    *nonce_size = 0;
    
    info("Getting SepNonce ");
    
    if (client->mode) {
        mode = client->mode->index;
    }
    
    switch (mode) {
        case MODE_NORMAL:
            info("in normal mode... ");
            if (normal_get_sep_nonce(client, nonce, nonce_size) < 0) {
                info("failed\n");
                return -1;
            }
            break;
        case MODE_DFU:
            info("in dfu mode... ");
            if (dfu_get_sep_nonce(client, nonce, nonce_size) < 0) {
                info("failed\n");
                return -1;
            }
            break;
        case MODE_RECOVERY:
            info("in recovery mode... ");
            if (recovery_get_sep_nonce(client, nonce, nonce_size) < 0) {
                info("failed\n");
                return -1;
            }
            break;
            
        default:
            info("failed\n");
            error("ERROR: Device is in an invalid state\n");
            return -1;
    }
    
    int i = 0;
    for (i = 0; i < *nonce_size; i++) {
        info("%02x ", (*nonce)[i]);
    }
    info("\n");
    
    return 0;
}

plist_t build_manifest_get_build_identity(plist_t build_manifest, uint32_t identity) {
    // fetch build identities array from BuildManifest
    plist_t build_identities_array = plist_dict_get_item(build_manifest, "BuildIdentities");
    if (!build_identities_array || plist_get_node_type(build_identities_array) != PLIST_ARRAY) {
        error("ERROR: Unable to find build identities node\n");
        return NULL;
    }
    
    // check and make sure this identity exists in buildmanifest
    if (identity >= plist_array_get_size(build_identities_array)) {
        return NULL;
    }
    
    plist_t build_identity = plist_array_get_item(build_identities_array, identity);
    if (!build_identity || plist_get_node_type(build_identity) != PLIST_DICT) {
        error("ERROR: Unable to find build identities node\n");
        return NULL;
    }
    
    return plist_copy(build_identity);
}

plist_t build_manifest_get_build_identity_for_model_with_restore_behavior(plist_t build_manifest, const char *hardware_model, const char *behavior)
{
    plist_t build_identities_array = plist_dict_get_item(build_manifest, "BuildIdentities");
    if (!build_identities_array || plist_get_node_type(build_identities_array) != PLIST_ARRAY) {
        error("ERROR: Unable to find build identities node\n");
        return NULL;
    }
    
    uint32_t i;
    for (i = 0; i < plist_array_get_size(build_identities_array); i++) {
        plist_t ident = plist_array_get_item(build_identities_array, i);
        if (!ident || plist_get_node_type(ident) != PLIST_DICT) {
            continue;
        }
        plist_t info_dict = plist_dict_get_item(ident, "Info");
        if (!info_dict || plist_get_node_type(ident) != PLIST_DICT) {
            continue;
        }
        plist_t devclass = plist_dict_get_item(info_dict, "DeviceClass");
        if (!devclass || plist_get_node_type(devclass) != PLIST_STRING) {
            continue;
        }
        char *str = NULL;
        plist_get_string_val(devclass, &str);
        if (strcasecmp(str, hardware_model) != 0) {
            free(str);
            continue;
        }
        free(str);
        str = NULL;
        if (behavior) {
            plist_t rbehavior = plist_dict_get_item(info_dict, "RestoreBehavior");
            if (!rbehavior || plist_get_node_type(rbehavior) != PLIST_STRING) {
                continue;
            }
            plist_get_string_val(rbehavior, &str);
            if (strcasecmp(str, behavior) != 0) {
                free(str);
                continue;
            } else {
                free(str);
                return plist_copy(ident);
            }
            free(str);
        } else {
            return plist_copy(ident);
        }
    }
    
    return NULL;
}

plist_t build_manifest_get_build_identity_for_model(plist_t build_manifest, const char *hardware_model)
{
    return build_manifest_get_build_identity_for_model_with_restore_behavior(build_manifest, hardware_model, NULL);
}

int get_tss_response(struct idevicerestore_client_t* client, plist_t build_identity, plist_t* tss) {
    plist_t request = NULL;
    plist_t response = NULL;
    *tss = NULL;
    
    if ((client->flags & FLAG_RERESTORE)) {
        error("checking for local shsh\n");
        
        /* first check for local copy */
        char zfn[1024];
        if (client->version) {
            if (client->cache_dir) {
                sprintf(zfn, "%s/shsh/" FMT_qu "-%s-%s-%s.shsh", client->cache_dir, (long long int)client->ecid, client->device->product_type, client->version, client->build);
            } else {
                sprintf(zfn, "shsh/" FMT_qu "-%s-%s-%s.shsh", (long long int)client->ecid, client->device->product_type, client->version, client->build);
            }
            struct stat fst;
            if (stat(zfn, &fst) == 0) {
                gzFile zf = gzopen(zfn, "rb");
                if (zf) {
                    int blen = 0;
                    int readsize = 16384;
                    int bufsize = readsize;
                    char* bin = (char*)malloc(bufsize);
                    char* p = bin;
                    do {
                        int bytes_read = gzread(zf, p, readsize);
                        if (bytes_read < 0) {
                            fprintf(stderr, "Error reading gz compressed data\n");
                            exit(EXIT_FAILURE);
                        }
                        blen += bytes_read;
                        if (bytes_read < readsize) {
                            if (gzeof(zf)) {
                                bufsize += bytes_read;
                                break;
                            }
                        }
                        bufsize += readsize;
                        bin = realloc(bin, bufsize);
                        p = bin + blen;
                    } while (!gzeof(zf));
                    gzclose(zf);
                    if (blen > 0) {
                        if (memcmp(bin, "bplist00", 8) == 0) {
                            plist_from_bin(bin, blen, tss);
                        } else {
                            plist_from_xml(bin, blen, tss);
                        }
                    }
                    free(bin);
                }
            } else {
                error("no local file %s\n", zfn);
            }
        } else {
            error("No version found?!\n");
        }
    }
    
    if (*tss) {
        info("Using local SHSH\n");
        return 0;
    }
    else if (client->flags & FLAG_RERESTORE) {
        info("Attempting to check Cydia TSS server for SHSH blobs\n");
        client->tss_url = strdup("http://cydia.saurik.com/TSS/controller?action=2");
    }
    else {
        info("Trying to fetch new SHSH blob\n");
    }
    
    /* populate parameters */
    plist_t parameters = plist_new_dict();
    plist_dict_set_item(parameters, "ApECID", plist_new_uint(client->ecid));
    if (client->nonce) {
        plist_dict_set_item(parameters, "ApNonce", plist_new_data((const char*)client->nonce, client->nonce_size));
    }
    unsigned char* sep_nonce = NULL;
    int sep_nonce_size = 0;
    get_sep_nonce(client, &sep_nonce, &sep_nonce_size);
    
    if (sep_nonce) {
        plist_dict_set_item(parameters, "ApSepNonce", plist_new_data((const char*)sep_nonce, sep_nonce_size));
        free(sep_nonce);
    }
    
    plist_dict_set_item(parameters, "ApProductionMode", plist_new_bool(1));
    if (client->image4supported) {
        plist_dict_set_item(parameters, "ApSecurityMode", plist_new_bool(1));
        plist_dict_set_item(parameters, "ApSupportsImg4", plist_new_bool(1));
    } else {
        plist_dict_set_item(parameters, "ApSupportsImg4", plist_new_bool(0));
    }
    
    tss_parameters_add_from_manifest(parameters, build_identity);
    
    /* create basic request */
    request = tss_request_new(NULL);
    if (request == NULL) {
        error("ERROR: Unable to create TSS request\n");
        plist_free(parameters);
        return -1;
    }
    
    /* add common tags from manifest */
    if (tss_request_add_common_tags(request, parameters, NULL) < 0) {
        error("ERROR: Unable to add common tags to TSS request\n");
        plist_free(request);
        plist_free(parameters);
        return -1;
    }
    
    /* add tags from manifest */
    if (tss_request_add_ap_tags(request, parameters, NULL) < 0) {
        error("ERROR: Unable to add common tags to TSS request\n");
        plist_free(request);
        plist_free(parameters);
        return -1;
    }
    
    if (client->image4supported) {
        /* add personalized parameters */
        if (tss_request_add_ap_img4_tags(request, parameters) < 0) {
            error("ERROR: Unable to add img4 tags to TSS request\n");
            plist_free(request);
            plist_free(parameters);
            return -1;
        }
    } else {
        /* add personalized parameters */
        if (tss_request_add_ap_img3_tags(request, parameters) < 0) {
            error("ERROR: Unable to add img3 tags to TSS request\n");
            plist_free(request);
            plist_free(parameters);
            return -1;
        }
    }
    
    if (client->mode->index == MODE_NORMAL) {
        /* normal mode; request baseband ticket aswell */
        plist_t pinfo = NULL;
        normal_get_preflight_info(client, &pinfo);
        if (pinfo) {
            plist_t node;
            node = plist_dict_get_item(pinfo, "Nonce");
            if (node) {
                plist_dict_set_item(parameters, "BbNonce", plist_copy(node));
            }
            node = plist_dict_get_item(pinfo, "ChipID");
            if (node) {
                plist_dict_set_item(parameters, "BbChipID", plist_copy(node));
            }
            node = plist_dict_get_item(pinfo, "CertID");
            if (node) {
                plist_dict_set_item(parameters, "BbGoldCertId", plist_copy(node));
            }
            node = plist_dict_get_item(pinfo, "ChipSerialNo");
            if (node) {
                plist_dict_set_item(parameters, "BbSNUM", plist_copy(node));
            }
            
            /* add baseband parameters */
            tss_request_add_baseband_tags(request, parameters, NULL);
        }
        client->preflight_info = pinfo;
    }
    
    /* send request and grab response */
    response = tss_request_send(request, client->tss_url);
    if (response == NULL) {
        info("ERROR: Unable to send TSS request\n");
        plist_free(request);
        plist_free(parameters);
        return -1;
    }
    
    info("Received SHSH blobs\n");
    if (client->flags & FLAG_RERESTORE) {
        client->tss_url = strdup("http://gs.apple.com/TSS/controller?action=2");
    }
    
    plist_free(request);
    plist_free(parameters);
    
    *tss = response;
    
    return 0;
}

void fixup_tss(plist_t tss)
{
    plist_t node;
    plist_t node2;
    node = plist_dict_get_item(tss, "RestoreLogo");
    if (node && (plist_get_node_type(node) == PLIST_DICT) && (plist_dict_get_size(node) == 0)) {
        node2 = plist_dict_get_item(tss, "AppleLogo");
        if (node2 && (plist_get_node_type(node2) == PLIST_DICT)) {
            plist_dict_remove_item(tss, "RestoreLogo");
            plist_dict_set_item(tss, "RestoreLogo", plist_copy(node2));
        }
    }
    node = plist_dict_get_item(tss, "RestoreDeviceTree");
    if (node && (plist_get_node_type(node) == PLIST_DICT) && (plist_dict_get_size(node) == 0)) {
        node2 = plist_dict_get_item(tss, "DeviceTree");
        if (node2 && (plist_get_node_type(node2) == PLIST_DICT)) {
            plist_dict_remove_item(tss, "RestoreDeviceTree");
            plist_dict_set_item(tss, "RestoreDeviceTree", plist_copy(node2));
        }
    }
    node = plist_dict_get_item(tss, "RestoreKernelCache");
    if (node && (plist_get_node_type(node) == PLIST_DICT) && (plist_dict_get_size(node) == 0)) {
        node2 = plist_dict_get_item(tss, "KernelCache");
        if (node2 && (plist_get_node_type(node2) == PLIST_DICT)) {
            plist_dict_remove_item(tss, "RestoreKernelCache");
            plist_dict_set_item(tss, "RestoreKernelCache", plist_copy(node2));
        }
    }
}

int build_manifest_get_identity_count(plist_t build_manifest) {
    // fetch build identities array from BuildManifest
    plist_t build_identities_array = plist_dict_get_item(build_manifest, "BuildIdentities");
    if (!build_identities_array || plist_get_node_type(build_identities_array) != PLIST_ARRAY) {
        error("ERROR: Unable to find build identities node\n");
        return -1;
    }
    
    // check and make sure this identity exists in buildmanifest
    return plist_array_get_size(build_identities_array);
}

int extract_component(const char* ipsw, const char* path, unsigned char** component_data, unsigned int* component_size)
{
    char* component_name = NULL;
    if (!ipsw || !path || !component_data || !component_size) {
        return -1;
    }
    
    component_name = strrchr(path, '/');
    if (component_name != NULL)
        component_name++;
    else
        component_name = (char*) path;
    
    info("Extracting %s...\n", component_name);
    if (ipsw_extract_to_memory(ipsw, path, component_data, component_size) < 0) {
        error("ERROR: Unable to extract %s from %s\n", component_name, ipsw);
        return -1;
    }
    
    return 0;
}

int personalize_component(const char *component_name, const unsigned char* component_data, unsigned int component_size, plist_t tss_response, unsigned char** personalized_component, unsigned int* personalized_component_size) {
    unsigned char* component_blob = NULL;
    unsigned int component_blob_size = 0;
    unsigned char* stitched_component = NULL;
    unsigned int stitched_component_size = 0;
    
    if (tss_response && tss_response_get_ap_img4_ticket(tss_response, &component_blob, &component_blob_size) == 0) {
        /* stitch ApImg4Ticket into IMG4 file */
        img4_stitch_component(component_name, component_data, component_size, component_blob, component_blob_size, &stitched_component, &stitched_component_size);
    } else {
        /* try to get blob for current component from tss response */
        if (tss_response && tss_response_get_blob_by_entry(tss_response, component_name, &component_blob) < 0) {
            debug("NOTE: No SHSH blob found for component %s\n", component_name);
        }
        
        if (component_blob != NULL) {
            if (img3_stitch_component(component_name, component_data, component_size, component_blob, 64, &stitched_component, &stitched_component_size) < 0) {
                error("ERROR: Unable to replace %s IMG3 signature\n", component_name);
                free(component_blob);
                return -1;
            }
        } else {
            info("Not personalizing component %s...\n", component_name);
            stitched_component = (unsigned char*)malloc(component_size);
            if (stitched_component) {
                stitched_component_size = component_size;
                memcpy(stitched_component, component_data, component_size);
            }
        }
    }
    free(component_blob);
    
    if (idevicerestore_keep_pers) {
        write_file(component_name, stitched_component, stitched_component_size);
    }
    
    *personalized_component = stitched_component;
    *personalized_component_size = stitched_component_size;
    return 0;
}

int build_manifest_check_compatibility(plist_t build_manifest, const char* product) {
    int res = -1;
    plist_t node = plist_dict_get_item(build_manifest, "SupportedProductTypes");
    if (!node || (plist_get_node_type(node) != PLIST_ARRAY)) {
        debug("%s: ERROR: SupportedProductTypes key missing\n", __func__);
        debug("%s: WARNING: If attempting to install iPhoneOS 2.x, be advised that Restore.plist does not contain the", __func__);
        debug("%s: WARNING: key 'SupportedProductTypes'. Recommendation is to manually add it to the Restore.plist.", __func__);
        return -1;
    }
    uint32_t pc = plist_array_get_size(node);
    uint32_t i;
    for (i = 0; i < pc; i++) {
        plist_t prod = plist_array_get_item(node, i);
        if (plist_get_node_type(prod) == PLIST_STRING) {
            char *val = NULL;
            plist_get_string_val(prod, &val);
            if (val && (strcmp(val, product) == 0)) {
                res = 0;
                free(val);
                break;
            }
        }
    }
    return res;
}

void build_manifest_get_version_information(plist_t build_manifest, struct idevicerestore_client_t* client) {
    plist_t node = NULL;
    client->version = NULL;
    client->build = NULL;
    
    node = plist_dict_get_item(build_manifest, "ProductVersion");
    if (!node || plist_get_node_type(node) != PLIST_STRING) {
        error("ERROR: Unable to find ProductVersion node\n");
        return;
    }
    plist_get_string_val(node, &client->version);
    
    node = plist_dict_get_item(build_manifest, "ProductBuildVersion");
    if (!node || plist_get_node_type(node) != PLIST_STRING) {
        error("ERROR: Unable to find ProductBuildVersion node\n");
        return;
    }
    plist_get_string_val(node, &client->build);
    
    client->build_major = strtoul(client->build, NULL, 10);
}

void build_identity_print_information(plist_t build_identity) {
    char* value = NULL;
    plist_t info_node = NULL;
    plist_t node = NULL;
    
    info_node = plist_dict_get_item(build_identity, "Info");
    if (!info_node || plist_get_node_type(info_node) != PLIST_DICT) {
        error("ERROR: Unable to find Info node\n");
        return;
    }
    
    node = plist_dict_get_item(info_node, "Variant");
    if (!node || plist_get_node_type(node) != PLIST_STRING) {
        error("ERROR: Unable to find Variant node\n");
        return;
    }
    plist_get_string_val(node, &value);
    
    info("Variant: %s\n", value);
    free(value);
    
    node = plist_dict_get_item(info_node, "RestoreBehavior");
    if (!node || plist_get_node_type(node) != PLIST_STRING) {
        error("ERROR: Unable to find RestoreBehavior node\n");
        return;
    }
    plist_get_string_val(node, &value);
    
    if (!strcmp(value, "Erase"))
        info("This restore will erase your device data.\n");
    
    if (!strcmp(value, "Update"))
        info("This restore will update your device without losing data.\n");
    
    free(value);
    
    info_node = NULL;
    node = NULL;
}

int build_identity_has_component(plist_t build_identity, const char* component) {
    plist_t manifest_node = plist_dict_get_item(build_identity, "Manifest");
    if (!manifest_node || plist_get_node_type(manifest_node) != PLIST_DICT) {
        return -1;
    }
    
    plist_t component_node = plist_dict_get_item(manifest_node, component);
    if (!component_node || plist_get_node_type(component_node) != PLIST_DICT) {
        return -1;
    }
    
    return 0;
}

int build_identity_get_component_path(plist_t build_identity, const char* component, char** path) {
    char* filename = NULL;
    
    plist_t manifest_node = plist_dict_get_item(build_identity, "Manifest");
    if (!manifest_node || plist_get_node_type(manifest_node) != PLIST_DICT) {
        error("ERROR: Unable to find manifest node\n");
        if (filename)
            free(filename);
        return -1;
    }
    
    plist_t component_node = plist_dict_get_item(manifest_node, component);
    if (!component_node || plist_get_node_type(component_node) != PLIST_DICT) {
        error("ERROR: Unable to find component node for %s\n", component);
        if (filename)
            free(filename);
        return -1;
    }
    
    plist_t component_info_node = plist_dict_get_item(component_node, "Info");
    if (!component_info_node || plist_get_node_type(component_info_node) != PLIST_DICT) {
        error("ERROR: Unable to find component info node for %s\n", component);
        if (filename)
            free(filename);
        return -1;
    }
    
    plist_t component_info_path_node = plist_dict_get_item(component_info_node, "Path");
    if (!component_info_path_node || plist_get_node_type(component_info_path_node) != PLIST_STRING) {
        error("ERROR: Unable to find component info path node for %s\n", component);
        if (filename)
            free(filename);
        return -1;
    }
    plist_get_string_val(component_info_path_node, &filename);
    
    *path = filename;
    return 0;
}

const char* get_component_name(const char* filename) {
    if (!strncmp(filename, "LLB", 3)) {
        return "LLB";
    } else if (!strncmp(filename, "iBoot", 5)) {
        return "iBoot";
    } else if (!strncmp(filename, "DeviceTree", 10)) {
        return "DeviceTree";
    } else if (!strncmp(filename, "applelogo", 9)) {
        return "AppleLogo";
    } else if (!strncmp(filename, "liquiddetect", 12)) {
        return "Liquid";
    } else if (!strncmp(filename, "recoverymode", 12)) {
        return "RecoveryMode";
    } else if (!strncmp(filename, "batterylow0", 11)) {
        return "BatteryLow0";
    } else if (!strncmp(filename, "batterylow1", 11)) {
        return "BatteryLow1";
    } else if (!strncmp(filename, "glyphcharging", 13)) {
        return "BatteryCharging";
    } else if (!strncmp(filename, "glyphplugin", 11)) {
        return "BatteryPlugin";
    } else if (!strncmp(filename, "batterycharging0", 16)) {
        return "BatteryCharging0";
    } else if (!strncmp(filename, "batterycharging1", 16)) {
        return "BatteryCharging1";
    } else if (!strncmp(filename, "batteryfull", 11)) {
        return "BatteryFull";
    } else if (!strncmp(filename, "needservice", 11)) {
        return "NeedService";
    } else if (!strncmp(filename, "SCAB", 4)) {
        return "SCAB";
    } else if (!strncmp(filename, "sep-firmware", 12)) {
        return "RestoreSEP";
    } else {
        error("WARNING: Unhandled component '%s'", filename);
        return filename;
    }
}
