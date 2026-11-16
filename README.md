build (static build):
```
docker build --target export --output type=local,dest=./dist .
```

build normally:
```
cmake -B build
cmake --build build
```