from random import shuffle

def generate_random_array(size):
	arr = range(0, size)
	shuffle(arr)

	return arr

def generate_test_data():
	data = []
	with open("test_data.c", "w") as t_data_file:
		size = 2

		while size < 4*1024*1024:
			data.append(generate_random_array(size))
			size *= 2
		
		str_data = str(data)
		str_data = str_data.replace("[","\n{")
		str_data = str_data.replace("]","}")
		t_data_file.write(str_data)


if __name__ == '__main__':
	generate_test_data()
