package progress

type Reporter interface {
	Start(label string, total int64) Span
}

type Span interface {
	SetTotal(total int64)
	Set(current int64, message string)
	Add(delta int64, message string)
	Message(message string)
	Done(message string)
	Fail(message string)
	Close()
}

func Use(reporter Reporter) Reporter {
	if reporter == nil {
		return nopReporter{}
	}
	return reporter
}

type nopReporter struct{}

func (nopReporter) Start(label string, total int64) Span {
	return nopSpan{}
}

type nopSpan struct{}

func (nopSpan) SetTotal(total int64)              {}
func (nopSpan) Set(current int64, message string) {}
func (nopSpan) Add(delta int64, message string)   {}
func (nopSpan) Message(message string)            {}
func (nopSpan) Done(message string)               {}
func (nopSpan) Fail(message string)               {}
func (nopSpan) Close()                            {}
