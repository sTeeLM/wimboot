/*
 * Copyright (C) 2014 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * @file
 *
 * EFI file system access
 *
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include "wimboot.h"
#include "vdisk.h"
#include "cmdline.h"
#include "wimpatch.h"
#include "wimfile.h"
#include "efi.h"
#include "efifile.h"

/** bootmgfw.efi path within WIM */
static const wchar_t bootmgfw_path[] = L"\\Windows\\Boot\\EFI\\bootmgfw.efi";

/** Other paths within WIM */
static const wchar_t *efi_wim_paths[] = {
	L"\\Windows\\Boot\\DVD\\EFI\\boot.sdi",
	L"\\Windows\\Boot\\DVD\\EFI\\BCD",
	L"\\Windows\\Boot\\Fonts\\segmono_boot.ttf",
	L"\\Windows\\Boot\\Fonts\\segoen_slboot.ttf",
	L"\\Windows\\Boot\\Fonts\\segoe_slboot.ttf",
	L"\\Windows\\Boot\\Fonts\\wgl4_boot.ttf",
	L"\\sms\\boot\\boot.sdi",
	NULL
};

/** bootmgfw.efi file */
struct vdisk_file *bootmgfw;

/**
 * Get architecture-specific boot filename
 *
 * @ret bootarch	Architecture-specific boot filename
 */
static const CHAR16 * efi_bootarch ( void ) {
	static const CHAR16 bootarch_full[] = EFI_REMOVABLE_MEDIA_FILE_NAME;
	const CHAR16 *tmp;
	const CHAR16 *bootarch = bootarch_full;

	for ( tmp = bootarch_full ; *tmp ; tmp++ ) {
		if ( *tmp == L'\\' )
			bootarch = ( tmp + 1 );
	}
	return bootarch;
}

/**
 * Read from EFI file
 *
 * @v vfile		Virtual file
 * @v data		Data buffer
 * @v offset		Offset
 * @v len		Length
 */
static void efi_read_file ( struct vdisk_file *vfile, void *data,
			    size_t offset, size_t len ) {
	EFI_FILE_PROTOCOL *file = vfile->opaque;
	UINTN size = len;
	EFI_STATUS efirc;

	/* Set file position */
	if ( ( efirc = file->SetPosition ( file, offset ) ) != 0 ) {
		die ( "Could not set file position: %#lx\n",
		      ( ( unsigned long ) efirc ) );
	}

	/* Read from file */
	if ( ( efirc = file->Read ( file, &size, data ) ) != 0 ) {
		die ( "Could not read from file: %#lx\n",
		      ( ( unsigned long ) efirc ) );
	}
}

/**
 * Patch BCD file
 *
 * @v vfile		Virtual file
 * @v data		Data buffer
 * @v offset		Offset
 * @v len		Length
 */
static void efi_patch_bcd ( struct vdisk_file *vfile __unused, void *data,
			    size_t offset, size_t len ) {
	static const wchar_t search[] = L".exe";
	static const wchar_t replace[] = L".efi";
	size_t i;

	/* Do nothing if BCD patching is disabled */
	if ( cmdline_rawbcd )
		return;

	/* Patch any occurrences of ".exe" to ".efi".  In the common
	 * simple cases, this allows the same BCD file to be used for
	 * both BIOS and UEFI systems.
	 */
	for ( i = 0 ; ( i + sizeof ( search ) ) < len ; i++ ) {
		if ( wcscasecmp ( ( data + i ), search ) == 0 ) {
			memcpy ( ( data + i ), replace, sizeof ( replace ) );
			DBG ( "...patched BCD at %#zx: \"%ls\" to \"%ls\"\n",
			      ( offset + i ), search, replace );
		}
	}
}


static int
isbootmgfw( const char *name)
{
	char bootarch[32];

	if (strcasecmp(name, "bootmgfw.efi") == 0)
		return 1;
	snprintf ( bootarch, sizeof ( bootarch ), "%ls", efi_bootarch() );
	return strcasecmp(name, bootarch) == 0;
}

static int
addfile( const char *name, void *data, size_t len,  void ( * read ) ( struct vdisk_file *file,
                                                       void *data,
                                                       size_t offset,
                                                       size_t len ) ) {
	struct vdisk_file *vfile;

	vfile = vdisk_add_file ( name, data, len, read );

        /* Check for special-case files */
	if ( isbootmgfw( name ) ) {
		DBG ( "...found bootmgfw.efi file %s\n", name );
		bootmgfw = vfile;
	} else if ( strcasecmp ( name, "BCD" ) == 0 ) {
		DBG ( "...found BCD\n" );
		vdisk_patch_file ( vfile, efi_patch_bcd );
	} else if ( strlen( name ) > 4 && strcasecmp ( ( name + ( strlen ( name ) - 4 ) ), ".wim" ) == 0 ) {
		DBG ( "...found WIM file %s\n", name );
		vdisk_patch_file ( vfile, patch_wim );
		if ( ( ! bootmgfw ) &&
		     ( bootmgfw = wim_add_file ( vfile, cmdline_index,
                                                         bootmgfw_path,
                                                         efi_bootarch() ) ) ) {
			DBG ( "...extracted %ls\n", bootmgfw_path );
		}
		wim_add_files ( vfile, cmdline_index, efi_wim_paths );
	}
	return 0;
}

static void read_file ( struct vdisk_file *file, void *data, size_t offset,
			size_t len ) {
	memcpy ( data, ( file->opaque + offset ), len );
}

/**
 * File handler
 *
 * @v name              File name
 * @v data              File data
 * @v len               Length
 * @ret rc              Return status code
 */
int
efi_add_file ( const char *name, void *data, size_t len)
{
	return addfile(name, data, len, read_file);
}



/**
 * Extract files from EFI file system
 *
 * @v handle		Device handle
 */
void efi_extract ( EFI_HANDLE handle ) {
	EFI_BOOT_SERVICES *bs = efi_systab->BootServices;
	union {
		EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
		void *interface;
	} fs;
	struct {
		EFI_FILE_INFO file;
		CHAR16 name[ VDISK_NAME_LEN + 1 /* WNUL */ ];
	} __attribute__ (( packed )) info;
	char name[ VDISK_NAME_LEN + 1 /* NUL */ ];
	struct vdisk_file *wim = NULL;
	struct vdisk_file *vfile;
	EFI_FILE_PROTOCOL *root;
	EFI_FILE_PROTOCOL *file;
	UINTN size;
	CHAR16 *wname;
	EFI_STATUS efirc;

	/* Open file system */
	if ( ( efirc = bs->OpenProtocol ( handle,
					  &efi_simple_file_system_protocol_guid,
					  &fs.interface, efi_image_handle, NULL,
					  EFI_OPEN_PROTOCOL_GET_PROTOCOL ))!=0){
		die ( "Could not open simple file system: %#lx\n",
		      ( ( unsigned long ) efirc ) );
	}

	/* Open root directory */
	if ( ( efirc = fs.fs->OpenVolume ( fs.fs, &root ) ) != 0 ) {
		die ( "Could not open root directory: %#lx\n",
		      ( ( unsigned long ) efirc ) );
	}

	/* Close file system */
	bs->CloseProtocol ( handle, &efi_simple_file_system_protocol_guid,
			    efi_image_handle, NULL );

	/* Read root directory */
	while ( 1 ) {

		/* Read directory entry */
		size = sizeof ( info );
		if ( ( efirc = root->Read ( root, &size, &info ) ) != 0 ) {
			die ( "Could not read root directory: %#lx\n",
			      ( ( unsigned long ) efirc ) );
		}
		if ( size == 0 )
			break;

		/* Ignore subdirectories */
		if ( info.file.Attribute & EFI_FILE_DIRECTORY )
			continue;

		/* Open file */
		wname = info.file.FileName;
		if ( ( efirc = root->Open ( root, &file, wname,
					    EFI_FILE_MODE_READ, 0 ) ) != 0 ) {
			die ( "Could not open \"%ls\": %#lx\n",
			      wname, ( ( unsigned long ) efirc ) );
		}

		/* Add file */
		snprintf ( name, sizeof ( name ), "%ls", wname );
		vfile = vdisk_add_file ( name, file, info.file.FileSize,
					 efi_read_file );

		/* Check for special-case files */
		if ( ( wcscasecmp ( wname, efi_bootarch() ) == 0 ) ||
		     ( wcscasecmp ( wname, L"bootmgfw.efi" ) == 0 ) ) {
			DBG ( "...found bootmgfw.efi file %ls\n", wname );
			bootmgfw = vfile;
		} else if ( wcscasecmp ( wname, L"BCD" ) == 0 ) {
			DBG ( "...found BCD\n" );
			vdisk_patch_file ( vfile, efi_patch_bcd );
		} else if ( wcscasecmp ( ( wname + ( wcslen ( wname ) - 4 ) ),
					 L".wim" ) == 0 ) {
			DBG ( "...found WIM file %ls\n", wname );
			wim = vfile;
		}
	}

	/* Process WIM image */
	if ( wim ) {
		vdisk_patch_file ( wim, patch_wim );
		if ( ( ! bootmgfw ) &&
		     ( bootmgfw = wim_add_file ( wim, cmdline_index,
						 bootmgfw_path,
						 efi_bootarch() ) ) ) {
			DBG ( "...extracted %ls\n", bootmgfw_path );
		}
		wim_add_files ( wim, cmdline_index, efi_wim_paths );
	}

	/* Check that we have a boot file */
	if ( ! bootmgfw ) {
		die ( "FATAL: no %ls or bootmgfw.efi found\n",
		      efi_bootarch() );
	}
}
