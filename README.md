# Fully Automatic Similarity Transformation (FAST)

An extensible and robust system for evaluating similarity joins across different domains and distance functions.  
This is the source code for the paper **Extensible and Robust Evaluation of Similarity Queries**.

## Compilation

### Run using Ansible

This section shows how FAST can be installed using the provided Ansible scripts.
The following assumptions are made on the machine:
* Up-to-date version of Debian (version 12 or higher)
* Root permissions
* The following standard dependencies are installed (using `apt install`):
  * git
  * ansible-core

1. Clone the repository
```
git clone https://github.com/DatabaseGroup/fast-similarity-evaluation.git fast-src
cd fast-src
```

2. Run the `fast` Ansible script on the local machine
```
ansible-playbook -i "localhost," -c local --extra-vars "home_dir=${HOME}"  ansible/fast.yml
```

3. Executables are copied to `${HOME}/experiments`
```
cd ${HOME}/experiments
```


### Run using Docker 

1. Build the docker image
```
docker build -t fast-similarity-evaluation:latest .
```

2. Run the docker image and mount datasets
```
docker run -it --rm  --volume /path/to/your/datasets/:/root/datasets fast-similarity-evaluation:latest
```

3. Executables are available at `/root/experiments`
```
cd /root/experiments
```

## Usage

FAST offers a command-line interface to configure data input, distance functions, and reduction selection, and more.

```
USAGE:
  -f [ --input-file ] arg                        Specify input file
  -h [ --shuffle ]                               Shuffle the dataset after parsing
  -w [ --warmup ]                                Perform warmup by filling caches before execution (only affects time-dynamic)
  --flush-hwcache                                Try to flush hardware (data-)caches between warmup and execution (only affects time-dynamic)
  -d [ --datatype ] arg                          Specify datatype (set, string, tree)
  -s [ --similarity ] arg                        Specify similarity function (jaccard, sed, ted, jaro)
  -t [ --threshold ] arg                         Threshold of the similarity join
  -b [ --batch-count ] arg (=20)                 Number of batches to split the data into, only affects block mode
  -l [ --label ] arg                             Label for the run (printed in json)
  -x [ --exclude-algorithm ] arg                 Excluded algorithms in the reduction graph
  -y [ --exclude-reduction ] arg                 Excluded reductions in the reduction graph
  -p [ --probe-cache-size ] arg (=20)            Probing signatures cache size, only affects block mode
  -r [ --reduction-cache-size ] arg (=20)        Reduction Cache Size, only affects block mode
  -u [ --read-until ] arg (=9223372036854775807) Read the first X lines of the input, skipping the rest
  -m [ --mode ] arg (=block)                     Mode of interleaving: block, time-static, time-dynamic
  -i [ --time-slice ] arg (=0.29999999999999999) Timeslice in seconds
  -a [ --additional-reductions ] arg             Enable optional reductions. Currently supported: qX enables X-grams
```

**Example**: Compute a *TED* join with *threshold* 42 using timeslices of length 0.2 seconds.
```
./fast -f /path/to/treedata \
    -d tree \
    -s ted \
    -t 42 \
    -l my-important-experiment \
    -m time-static \
    -i 0.2
```

### Dataset Format

FAST supports sets, strings, and trees.

#### Sets

A line corresponds to a set. Each set consists of numerical tokens separated by spaces.

**Example** (3 sets): 
```
1 103 5 42
2 3 1
5 1 40 38
```

#### Strings

A line corresponds to a string.

**Example** (3 strings):
```
extensibility
efficiency
robustness
```

#### Trees

A line corresponds to a tree. Trees are encoded in *bracket notation*. For example, `{A{B{X}{Y}{F}}{C}}` corresponds to
```
    A
   / \
  B   C
 /|\
X Y F
```
