export class Vector2 {
  /**
   * @property x
   * @type Number
   * @default 0
   */
  public x: number

  /**
   * @property y
   * @type Number
   * @default 0
   */
  public y: number

  constructor(x?: number, y?: number) {
    this.x = x ?? 0
    this.y = y ?? x ?? 0
  }

  public add(other: Vector2): Vector2 {
    this.x += other.x
    this.y += other.y
    return this
  }

  public sub(other: Vector2): Vector2 {
    this.x -= other.x
    this.y -= other.y
    return this
  }

  public multiply(other: Vector2): Vector2 {
    this.x *= other.x
    this.y *= other.y
    return this
  }

  public divide(other: Vector2): Vector2 {
    this.x /= other.x
    this.y /= other.y
    return this
  }

  public invert(): Vector2 {
    this.x = -this.x
    this.y = -this.y
    return this
  }

  public magnitude(): number {
    return Math.sqrt(this.x * this.x + this.y * this.y)
  }

  get Angle() {
    return Math.atan2(this.y, this.x)
  }

  get AngleInDegrees() {
    return (Math.atan2(this.y, this.x) * 180) / Math.PI
  }

  public normalize(): Vector2 {
    const length = this.magnitude()
    if (length === 0) {
      return this
    }
    this.divide(new Vector2(length, length))
    return this
  }

  static fromPoint(point: { x: number; y: number }): Vector2 {
    return new Vector2(point.x, point.y)
  }

  toString(): string {
    return `${this.x}, ${this.y}`
  }
}
