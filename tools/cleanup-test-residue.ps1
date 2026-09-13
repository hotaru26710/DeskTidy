# ---------------------------------------------------------------------------
# cleanup-test-residue.ps1 -- purge test leftovers from the recycle bin.
#
# WHY THIS IS A SEPARATE .ps1 INSTEAD OF INLINE IN verify.bat:
#   It used to be an inline multi-line "powershell -Command ... ^" block.
#   That form broke cmd's parser -- the command contained parentheses, which
#   cmd treats as block delimiters, so it began executing the following lines
#   as commands. The REM '-----' rules were then parsed as command names and
#   it spun in an endless error loop. As a separate file, cmd sees one simple
#   command and parses nothing else.
#
# WHY THE FILE IS SAVED AS UTF-8 WITH BOM:
#   Windows PowerShell reads a BOM-less file as ANSI. The Chinese comments in
#   this file would then be mis-decoded and their bytes can be parsed as
#   syntax (it produced a bogus "Missing closing ')'" error). The BOM makes
#   PowerShell read it as UTF-8.
#
# SCOPE: deleteBox unit tests and tools/delete_probe really move box
#   directories into the recycle bin, and QTemporaryDir cannot reclaim them.
#   Only directories carrying a known test prefix are touched.
# ---------------------------------------------------------------------------

$ErrorActionPreference = 'SilentlyContinue'

$prefixes = @(
    'DeskTidyTest-',
    'DeskTidyDelProbe-',
    'e2eDelTest-'
)

$removed = 0
try {
    $shell = New-Object -ComObject Shell.Application
    $bin = $shell.Namespace(10)
    if ($null -eq $bin) {
        Write-Host '  recycle bin not available, skipping cleanup'
        exit 0
    }

    foreach ($item in @($bin.Items())) {
        $name = $item.Name
        foreach ($p in $prefixes) {
            if ($name -like ($p + '*')) {
                $item.InvokeVerb('delete')
                $removed++
                break
            }
        }
    }
} catch {
    Write-Host '  cleanup skipped due to an error'
    exit 0
}

if ($removed -gt 0) {
    Write-Host ('  removed ' + $removed + ' test item from recycle bin')
} else {
    Write-Host '  nothing to clean'
}