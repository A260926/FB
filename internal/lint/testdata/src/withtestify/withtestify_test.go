package withtestify

import (
	_ "github.com/stretchr/testify/assert"  // want `testify import github.com/stretchr/testify/assert — consider migrating to gotest`
	_ "github.com/stretchr/testify/require" // want `testify import github.com/stretchr/testify/require — consider migrating to gotest`
)
