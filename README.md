# Simple File System
This project is project that implements a Simple File System. It is written in C.

## Feature
- It provides mount, open, read, write, and close functionalities.
- It provides a disk image creation tool.
- It provides directory entry cache & hash and buffer cache functionalities.

## Project Structure
```
2025_os_proj3/
├── 00_Document/ # This directory is directory that contains documentation-related files.
│ ├── 00_Assignment4.pdf
│ └── 01_Assignment4 - Simple File System (Yoo, J. H., 32212808, Department of Mobile Systems Engineering).pdf
├── 01_src/      # This directory is directory that source code-related files.
│ ├── fs.h
│ ├── main.c
│ └── Makefile
├── LICENSE
└── README.md
```

## Installation
1. Clone the repository
	```
    gh repo clone mobile-os-dku-cis-mse/2025_os_proj3
	```

2. Navigate to the project directory
	```
	cd 2025_os_proj3/01_src
	```

## Usage
1. Build the project
	```
    make
	```

2. Run the program
	```
	./ main
	```

3. Make file system
	```
	mkfs [file]
	```

4. Mount
	```
	mount [file]
	```

5. Write
	```
	write [file] [data]
	```

6. cat
	```
	cat [file]
	```

7. ls
	```
	ls
	```

8. Set cache
	```
	set_cache [dentry|buffer] [on|off]
	```

9. Cache stat
	```
	cache_stat
	```

10. Exit
	```
	exit
	```

## Test
I tested the non-functional requirements described in document.
- Tested execution time by receiving arguments to enable or disable the directory entry cache & hash and buffer cache, and measured performance in four cases:
    - Directory entry cache & hash disabled + buffer cache disabled
    - Directory entry cache & hash disabled + buffer cache enabled
    - Directory entry cache & hash enabled + buffer cache disabled
    - Directory entry cache & hash enabled + buffer cache enabled
- Tested error handling by providing invalid input arguments and verifying that appropriate error messages are printed.

## Contributing
1. Create a new branch.
	```
	git checkout -b your_branch
	```

2. Commit your changes.
	```
	git add .
	git commit -m "Commit message"
	```

3. Push to the branch.
	```
	git push origin your_branch
	```

4. Open a pull request.

## License
This project is licensed under the MIT License.

## Authors
- Yoo, J. H. ([Yoo, J. H.](https://github.com/YooJunHyuk123))
- Email: a01091040305@gmail.com