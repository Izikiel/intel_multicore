from random import shuffle


def generate_random_array(size):
    arr = range(0, size)
    shuffle(arr)

    return arr


def generate_test_data():
    data = []
    with open("test_data.c", "w") as t_data_file:
        size = 2
        i = 0
        while size < 4 * 1024: # * 1024:
            data.append(generate_string_array(i, size))
            size *= 2
            i += 1

        header = "#include \"test_data.h\"\n"
        assign = "uint32_t* test_data[%d] = {"%len(data)
        str_data = header + "".join(data) + assign + \
                   "".join(["test%d,"%j for j in xrange(len(data))]) + "};"
        t_data_file.write(str_data)

    with open("test_data.h","w") as header_file:
        header_file.write("#include \"types.h\"\n\n")
        header_file.write("extern uint32_t* test_data[];")


def generate_string_array(i, size):
    arr = generate_random_array(size)
    arr = str(arr)
    arr = arr.replace("[", "{")
    arr = arr.replace("]", "}")
    arr += ";\n"

    header = "static uint32_t test%d[%d] = " % (i, size)

    arr = header + arr

    return arr


if __name__ == '__main__':
    generate_test_data()

#gcc -std=c99 -m64 -g -ggdb -Wall -Werror -O0 -fno-zero-initialized-in-bss -fno-stack-protector -ffreestanding -I../include -c -o t test_data.c