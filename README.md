# JIGSAW

**Build**
```
mkdir build 
cd build && make
```

**Using Docker**

```
docker build -t jigsaw-test .

# copy constraints files to /out/readelf inside the container
docker run jigsaw-test /src/jigsaw/build/rgd 1 0 0 0 0 /out/readelf
```

**Replay from constraints files**
```
Command:
./rgd num_of_threads pin_core_start test_dir

Example:
# solve objdump constraints using 8 cores, starting from core 0
./rgd 8 0 objdump 
```

**Constraints Files**

https://drive.google.com/drive/folders/1fxSvKI7C3c2mLqcGN9H1RJ1aHkeKEVxQ


