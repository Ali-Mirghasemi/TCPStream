# TCPStream

This library implement [Stream][1] driver for Linux over tcp connection

## Usage
Clone current repository and [Stream][1] alongside each other for simpilicity like this
```
- ./<root>
  |_ Stream
  |_ TCPStream
```

**Example:**

```
mkdir project
cd project
git clone https://github.com/Alii-Mirghasemi/Stream.git
git clone https://github.com/Alii-Mirghasemi/TCPStream.git
```

**Build Examples**

```
cd TCPStream
cmake -S. -Bbuild -DTCPSTREAM_BUILD_EXAMPLES=ON
cmake --build build --config Release
```

**Run Server**

```
./bild/Examples/TCPStream-ServerBasic
```


**Run Client**

```
./bild/Examples/TCPStream-Basic
```


[1]: https://github.com/Alii-Mirghasemi/Stream

