# Ytingest

Ytingest enables LLMs to gain knowledge from YouTube video content. Ytingest allows you to:

1. Retrieve transcript from YouTube videos (when available)
1. Save the information in a format compatible with LLMs
1. Calculate token counts using an OpenAI model

## Build and Usage

Requirements:

1. [libcurl](https://curl.se/libcurl) for http request
1. [cargo](https://www.rust-lang.org/tools/install) for tiktoken-rs compilation

Run the following command to clone and update submodules:

```sh
git clone --recurse-submodules https://github.com/ryhkml/ytingest
git submodule update --init --recursive
```

Then initiate the build process:

```sh
cc -o nob nob.c
./nob
```

The compiled executable will be located in the `out` directory. You can run it using the following command:

```
out/ytingest "https://www.youtube.com/watch?v=OeYnV9zp7Dk"
```

There are options listed:

| Option                | Default Value | Description                                                                          |
| --------------------- | ------------- | ------------------------------------------------------------------------------------ |
| `-e`, `--excludes`    |               | Specify YouTube metadata fields to exclude (comma-separated)                         |
| `--format`            | txt           | Specify the output file format. Available: json, md, and txt                         |
| `--lang`              | en            | Specify language code for transcript translation                                     |
| `--lang-available`    |               | Display available transcript translation languages                                   |
| `-O`, `--output-path` | ingest        | Specify the directory to save the output file                                        |
| `-T`, `--token-count` |               | Specify Google Gemini or OpenAI model name to estimate the token count of the output |
| `-h`, `--help`        |               | Display help message and exit                                                        |
| `-v`, `--version`     |               | Display help message and exit                                                        |

> [!NOTE]
>
> If ytingest cannot access the shared object file, set the `LD_LIBRARY_PATH` env temporarily or permanently by running the following command in the project directory:
>
> ```sh
> export LD_LIBRARY_PATH="$(pwd)/src/tiktoken-c/target/release:$LD_LIBRARY_PATH"
> ```

## Formatter

`.clang-format` is based on Google style guide

```
BasedOnStyle: Google
IndentWidth: 4
ColumnLimit: 120
AlignArrayOfStructures: Left
AlignAfterOpenBracket: Align
BracedInitializerIndentWidth: 4
```
