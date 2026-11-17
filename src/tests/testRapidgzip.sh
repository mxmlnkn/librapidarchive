#!/usr/bin/env bash

# Should be run from build folder


RAPIDGZIP='src/tools/rapidgzip'
export RAPIDGZIP

function echoerr() { echo "$@" 1>&2; }

returnError()
{
    local lineNumber message
    if [ $# -eq 2 ]; then
        lineNumber=:$1
        message=$2
    else
        message=$*
    fi

    echoerr -e "\e[37m${FUNCNAME[1]}$lineNumber <- ${FUNCNAME[*]:2}\e[0m"
    echoerr -e "\e[37m$message\e[0m"
    echoerr -e '\e[31mTEST FAILED!\e[0m'

    exit 1
}


# Tests with stdin are hard to do in Catch2, so do them here.
[[ "$( cat ../src/tests/data/base64-256KiB.gz | $RAPIDGZIP --count )" -eq 262144 ]] ||
    returnError "$LINENO" 'Stdin count test failed!'

# Header strings must not inject terminal escape sequences into --analyze output.
tmpFolder=$( mktemp -d )
trap 'rm -rf "$tmpFolder"' EXIT
python3 - "$tmpFolder/escaped-name.gz" "$tmpFolder/backslash-name.gz" <<'PYTHON'
import gzip
import sys

for path, name in zip( sys.argv[1:], [ '\x1b[31mred\x1b[0m\nnext', r'literal\x1b' ] ):
    with open( path, 'wb' ) as file:
        with gzip.GzipFile( fileobj=file, mode='wb', filename=name ) as gzipFile:
            gzipFile.write( b'hello' )
PYTHON
analysis=$( $RAPIDGZIP --analyze "$tmpFolder/escaped-name.gz" )
[[ "$analysis" == *'\x1b[31mred\x1b[0m\x0anext'* ]] ||
    returnError "$LINENO" 'The gzip file name was not escaped in the --analyze output!'
[[ "$analysis" != *$'\x1b'* ]] ||
    returnError "$LINENO" 'The gzip file name injected a terminal escape sequence into the --analyze output!'
analysis=$( $RAPIDGZIP --analyze "$tmpFolder/backslash-name.gz" )
[[ "$analysis" == *'literal\\x1b'* ]] ||
    returnError "$LINENO" 'A literal backslash in the gzip file name was not escaped in the --analyze output!'

# Exercise the index paths, which open all files before the decoder starts.
tmpOutput="$tmpFolder/base64-256KiB"
$RAPIDGZIP -d -f -o "$tmpOutput" --export-index "$tmpFolder/base64.index" ../src/tests/data/base64-256KiB.gz ||
    returnError "$LINENO" 'Exporting an index failed!'
[[ "$( wc -c < "$tmpOutput" )" -eq 262144 ]] ||
    returnError "$LINENO" 'Decompressing with index export produced the wrong size!'
$RAPIDGZIP -d -f -o /dev/null --import-index "$tmpFolder/base64.index" ../src/tests/data/base64-256KiB.gz ||
    returnError "$LINENO" 'Importing an index failed!'

echo -e '\e[32mAll tests ran successfully.\e[0m'
