# sortedmap
The sortedmap package provides some utility methods to sort maps by keys or values

See documentation at https://godoc.org/github.com/gobs/sortedmap

## Usage
The most common use cases is to print the content of a map sorted by key:

	unsorted := map[string]interface{}{
		"b": 2.0,
		"a": 1,
		"c": true,
		"e": nil,
		"d": "four",
	}

	fmt.Println(sortedmap.AsSortedMap(unsorted))	

	// or

	for _, ele := range sortedmap.AsSortedMap(unsorted) {
		fmt.Println(ele)    // implements Stringer
		
		fmt.Println(ele.Key + ":" + ele.Value)
	}

You can also generate a JSON object with sorted keys:

	jbuffer, _ := json.MarshalIndent(sortedmap.AsSortedMap(unsorted), "", "  ")
	fmt.Println(string(jbuffer)
