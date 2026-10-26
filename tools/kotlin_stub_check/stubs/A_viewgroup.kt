package android.view
interface ViewGroupLayoutParams {
    var width: Int
    var height: Int
}
class ViewGroupLayoutParamsImpl(val w: Int, val h: Int) : ViewGroupLayoutParams {
    override var width: Int = w
    override var height: Int = h
}
