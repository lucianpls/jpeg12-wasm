rm -f *.o

OPTIONS="-O3 -flto"
for file in jpeg12-6b/*.c
do
    echo $file
    emcc $OPTIONS -c $file
done

echo jpeg12api.cpp

# define IGNORE_ZEN_CHUNK when compiling jpeg12api to disable zen chunk processing
# it makes decoding about 5% faster, but zero values are not stable
emcc $OPTIONS -c jpeg12api.cpp Packer_RLE.cpp

echo Building jpeg12dec

EXPORTS=_malloc,_free

METHODS=[cwrap,UTF8ToString,writeArrayToMemory]

PARAMS="-sNO_EXIT_RUNTIME=1 --no-entry\
 -Wshift-negative-value\
 -sEXPORTED_FUNCTIONS=$EXPORTS\
 -sEXPORTED_RUNTIME_METHODS=$METHODS"
echo emcc $OPTIONS $PARAMS -o jpeg12dec.js *.o
emcc $OPTIONS $PARAMS -o jpeg12dec.js *.o

rm -f *.o
